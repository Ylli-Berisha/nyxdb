#include "executor/table_scan.h"

#include "storage/disk/column_file.h"
#include "storage/disk/column_page.h"
#include "storage/disk/page.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>
#include <variant>

namespace nyx {

static void copy_null_bits(u8* dest, size_t dest_bit_off, const u8* src, size_t src_bit_off,
                           size_t count, ColumnVector& out) {
    bool any = false;
    for (size_t i = 0; i < count; ++i) {
        size_t s = src_bit_off + i;
        bool bit = (src[s / 8] >> (s % 8)) & 1u;
        if (bit) {
            size_t d = dest_bit_off + i;
            dest[d / 8] |= static_cast<u8>(1u << (d % 8));
            any = true;
        }
    }
    if (any)
        out.set_has_nulls(true);
}

static bool matches_type(const Value& v, TypeId t) {
    switch (t) {
    case TypeId::INT32:
        return std::holds_alternative<i32>(v);
    case TypeId::INT64:
        return std::holds_alternative<i64>(v);
    case TypeId::DOUBLE:
        return std::holds_alternative<f64>(v);
    case TypeId::VARCHAR:
        return std::holds_alternative<std::string>(v);
    default:
        return false;
    }
}

static bool page_overlaps_range(const ColumnPage& p, const ScanRange& r, TypeId type) {
    switch (type) {
    case TypeId::INT32: {
        auto pmin = p.min_i32();
        auto pmax = p.max_i32();
        if (!pmin.has_value() || !pmax.has_value())
            return false;
        if (r.lo.has_value() && *pmax < std::get<i32>(*r.lo))
            return false;
        if (r.hi.has_value() && *pmin > std::get<i32>(*r.hi))
            return false;
        return true;
    }
    case TypeId::INT64: {
        auto pmin = p.min_i64();
        auto pmax = p.max_i64();
        if (!pmin.has_value() || !pmax.has_value())
            return false;
        if (r.lo.has_value() && *pmax < std::get<i64>(*r.lo))
            return false;
        if (r.hi.has_value() && *pmin > std::get<i64>(*r.hi))
            return false;
        return true;
    }
    case TypeId::DOUBLE: {
        auto pmin = p.min_f64();
        auto pmax = p.max_f64();
        if (!pmin.has_value() || !pmax.has_value())
            return false;
        if (r.lo.has_value() && *pmax < std::get<f64>(*r.lo))
            return false;
        if (r.hi.has_value() && *pmin > std::get<f64>(*r.hi))
            return false;
        return true;
    }
    default:
        return true;
    }
}

TableScan::TableScan(Table* table, std::vector<size_t> projected)
    : table_(table), projected_(std::move(projected)) {
    assert(table_ != nullptr);
    output_schema_.reserve(projected_.size());
    for (size_t idx : projected_) {
        assert(idx < table_->column_count());
        output_schema_.push_back(table_->schema()[idx]);
    }
}

TableScan::TableScan(Table* table, std::vector<size_t> projected, ScanRange range)
    : TableScan(table, std::move(projected)) {
    range_ = std::move(range);
    assert(range_->col_idx < table_->column_count());
    TypeId ct = table_->schema()[range_->col_idx].type;
    (void)ct;
    assert(!range_->lo.has_value() || matches_type(*range_->lo, ct));
    assert(!range_->hi.has_value() || matches_type(*range_->hi, ct));
}

static void refine_survivors_by_deletions(std::vector<std::pair<u64, u64>>& survivors,
                                          const std::vector<u8>& bm) {
    std::vector<std::pair<u64, u64>> refined;
    for (auto [start, end] : survivors) {
        u64 r = start;
        while (r < end) {
            while (r < end) {
                usize byte_idx = r / 8;
                if (byte_idx >= bm.size() || !((bm[byte_idx] >> (r % 8)) & 1u))
                    break;
                ++r;
            }
            if (r >= end)
                break;
            u64 s = r;
            while (r < end) {
                usize byte_idx = r / 8;
                if (byte_idx < bm.size() && ((bm[byte_idx] >> (r % 8)) & 1u))
                    break;
                ++r;
            }
            refined.emplace_back(s, r);
        }
    }
    survivors = std::move(refined);
}

Result<void> TableScan::build_entry(SegScanState& st) {
    if (range_.has_value()) {
        ColumnFile& range_col =
            st.seg ? st.seg->column(range_->col_idx) : table_->column(range_->col_idx);
        TypeId type = range_col.type();
        u64 cursor = 0;
        auto rr = range_col.scan([&](const ColumnPage& p) {
            u16 vc = p.value_count();
            u64 start = cursor;
            u64 end = cursor + vc;
            cursor = end;
            if (vc == 0)
                return;
            if (page_overlaps_range(p, *range_, type)) {
                if (!st.survivors.empty() && st.survivors.back().second == start)
                    st.survivors.back().second = end;
                else
                    st.survivors.emplace_back(start, end);
            }
        });
        if (rr.is_err())
            return Result<void>::err(rr.error().message);
    } else {
        if (st.local_row_count > 0)
            st.survivors.emplace_back(0, st.local_row_count);
    }

    const auto& bm = st.seg ? st.seg->deleted_bitmap() : table_->deleted_bitmap();
    if (!bm.empty())
        refine_survivors_by_deletions(st.survivors, bm);

    return Result<void>::ok();
}

Result<void> TableScan::open() {
    lock_ = table_->lock_shared();
    scan_plan_.clear();
    current_seg_ = 0;

    const auto& segs = table_->segments();
    scan_plan_.reserve(segs.size() + 1);

    for (usize i = 0; i < segs.size(); ++i) {
        Segment* seg = const_cast<Segment*>(&segs[i]);
        SegScanState st;
        st.seg = seg;
        st.base_row_id = seg->meta().base_row_id;
        st.local_row_count = seg->meta().row_count;
        auto r = build_entry(st);
        if (r.is_err())
            return r;
        scan_plan_.push_back(std::move(st));
    }

    {
        u64 wb_count =
            table_->wb_columns_ref().empty() ? 0 : table_->wb_columns_ref()[0].row_count();
        SegScanState st;
        st.seg = nullptr;
        st.base_row_id = table_->wb_base_row_id();
        st.local_row_count = wb_count;
        auto r = build_entry(st);
        if (r.is_err())
            return r;
        scan_plan_.push_back(std::move(st));
    }

    opened_ = true;
    return Result<void>::ok();
}

void TableScan::close() {
    lock_ = {};
    opened_ = false;
}

Result<std::optional<Chunk>> TableScan::next() {
    assert(opened_);

    while (current_seg_ < scan_plan_.size()) {
        SegScanState& st = scan_plan_[current_seg_];

        while (st.range_idx < st.survivors.size() &&
               st.cur_local >= st.survivors[st.range_idx].second)
            ++st.range_idx;

        if (st.range_idx >= st.survivors.size()) {
            ++current_seg_;
            continue;
        }

        auto [range_start, range_end] = st.survivors[st.range_idx];
        if (st.cur_local < range_start)
            st.cur_local = range_start;

        u64 remaining = range_end - st.cur_local;
        size_t count = static_cast<size_t>(std::min<u64>(CHUNK_SIZE, remaining));

        u64 local_chunk_start = st.cur_local;
        last_chunk_physical_start_ = st.base_row_id + local_chunk_start;

        std::vector<ColumnVector> cols;
        cols.reserve(projected_.size());
        for (size_t col_idx : projected_) {
            auto res = read_col(st, col_idx, local_chunk_start, count);
            if (res.is_err())
                return Result<std::optional<Chunk>>::err(res.error().message);
            cols.push_back(std::move(res.value()));
        }

        st.cur_local += count;
        return Result<std::optional<Chunk>>::ok(Chunk(count, std::move(cols)));
    }

    return Result<std::optional<Chunk>>::ok(std::nullopt);
}

Result<ColumnVector> TableScan::read_col(SegScanState& st, size_t col_idx, u64 local_start,
                                         size_t count) {
    ColumnFile& cf = st.seg ? st.seg->column(col_idx) : table_->column(col_idx);
    u16 capacity = cf.page_capacity();
    u64 remaining = count;
    u64 cursor = local_start;

    if (cf.type() == TypeId::VARCHAR) {
        ColumnVector out = ColumnVector::empty(TypeId::VARCHAR, cf.nullable(), count);
        while (remaining > 0) {
            PageId page_id = static_cast<PageId>(cursor / capacity);
            u16 slot_in_page = static_cast<u16>(cursor % capacity);

            auto rp = cf.read_page(page_id);
            if (rp.is_err())
                return Result<ColumnVector>::err(rp.error().message);

            const ColumnPage view(*rp.value());
            u16 avail = static_cast<u16>(view.value_count() - slot_in_page);
            u16 take = static_cast<u16>(std::min<u64>(avail, remaining));

            for (u16 j = 0; j < take; ++j) {
                u16 slot = static_cast<u16>(slot_in_page + j);
                if (cf.nullable() && view.is_null(slot))
                    out.append_null();
                else
                    out.append_str(view.get_str(slot));
            }

            cursor += take;
            remaining -= take;
        }
        return Result<ColumnVector>::ok(std::move(out));
    }

    ColumnVector out = ColumnVector::make(cf.type(), count, cf.nullable());
    size_t type_bytes = type_size(cf.type());
    size_t out_offset = 0;

    while (remaining > 0) {
        PageId page_id = static_cast<PageId>(cursor / capacity);
        u16 slot_in_page = static_cast<u16>(cursor % capacity);

        auto rp = cf.read_page(page_id);
        if (rp.is_err())
            return Result<ColumnVector>::err(rp.error().message);

        const ColumnPage view(*rp.value());
        u16 avail = static_cast<u16>(view.value_count() - slot_in_page);
        u16 take = static_cast<u16>(std::min<u64>(avail, remaining));

        std::memcpy(out.data() + out_offset * type_bytes,
                    view.value_area() + static_cast<size_t>(slot_in_page) * type_bytes,
                    static_cast<size_t>(take) * type_bytes);

        if (cf.nullable()) {
            copy_null_bits(out.null_bitmap_data(), out_offset, view.null_bitmap(), slot_in_page,
                           take, out);
        }

        out_offset += take;
        cursor += take;
        remaining -= take;
    }
    return Result<ColumnVector>::ok(std::move(out));
}

} // namespace nyx
