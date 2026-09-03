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

Result<void> TableScan::open() {
    if (!range_.has_value()) {
        next_row_ = 0;
        total_rows_ = table_->row_count();
        opened_ = true;
        return Result<void>::ok();
    }
    auto r = compute_survivors();
    if (r.is_err())
        return r;
    cur_range_ = 0;
    cur_row_ = survivors_.empty() ? 0 : survivors_[0].first;
    opened_ = true;
    return Result<void>::ok();
}

Result<void> TableScan::compute_survivors() {
    survivors_.clear();
    assert(range_.has_value());
    const ScanRange& r = *range_;
    ColumnFile& cf = table_->column(r.col_idx);
    TypeId type = cf.type();
    u64 cursor = 0;

    auto rr = cf.scan([&](const ColumnPage& p) {
        u16 vc = p.value_count();
        u64 start = cursor;
        u64 end = cursor + vc;
        cursor = end;
        if (vc == 0)
            return;
        if (page_overlaps_range(p, r, type)) {
            if (!survivors_.empty() && survivors_.back().second == start)
                survivors_.back().second = end;
            else
                survivors_.push_back({start, end});
        }
    });
    if (rr.is_err())
        return Result<void>::err(rr.error().message);
    return Result<void>::ok();
}

Result<std::optional<Chunk>> TableScan::next() {
    assert(opened_);
    if (!range_.has_value())
        return next_no_range();
    return next_with_survivors();
}

Result<std::optional<Chunk>> TableScan::next_no_range() {
    if (next_row_ >= total_rows_)
        return Result<std::optional<Chunk>>::ok(std::nullopt);

    size_t count = static_cast<size_t>(std::min<u64>(CHUNK_SIZE, total_rows_ - next_row_));
    auto cols = read_projected_columns(next_row_, count);
    if (cols.is_err())
        return Result<std::optional<Chunk>>::err(cols.error().message);

    next_row_ += count;
    return Result<std::optional<Chunk>>::ok(Chunk(count, std::move(cols.value())));
}

Result<std::optional<Chunk>> TableScan::next_with_survivors() {
    while (cur_range_ < survivors_.size()) {
        auto [start, end] = survivors_[cur_range_];
        (void)start;
        if (cur_row_ >= end) {
            ++cur_range_;
            if (cur_range_ < survivors_.size())
                cur_row_ = survivors_[cur_range_].first;
            continue;
        }
        u64 remaining_in_range = end - cur_row_;
        size_t count = static_cast<size_t>(std::min<u64>(CHUNK_SIZE, remaining_in_range));

        auto cols = read_projected_columns(cur_row_, count);
        if (cols.is_err())
            return Result<std::optional<Chunk>>::err(cols.error().message);

        cur_row_ += count;
        return Result<std::optional<Chunk>>::ok(Chunk(count, std::move(cols.value())));
    }
    return Result<std::optional<Chunk>>::ok(std::nullopt);
}

Result<std::vector<ColumnVector>> TableScan::read_projected_columns(u64 start, size_t count) {
    std::vector<ColumnVector> cols;
    cols.reserve(projected_.size());
    for (size_t col_idx : projected_) {
        auto res = read_column_range(col_idx, start, count);
        if (res.is_err())
            return Result<std::vector<ColumnVector>>::err(res.error().message);
        cols.push_back(std::move(res.value()));
    }
    return Result<std::vector<ColumnVector>>::ok(std::move(cols));
}

Result<ColumnVector> TableScan::read_column_range(size_t col_idx, u64 start, size_t count) {
    ColumnFile& cf = table_->column(col_idx);
    ColumnVector out = ColumnVector::make(cf.type(), count, cf.nullable());
    size_t type_bytes = type_size(cf.type());
    u16 capacity = cf.page_capacity();

    u64 remaining = count;
    u64 cursor = start;
    size_t out_offset = 0;

    Page page_buf{};
    while (remaining > 0) {
        PageId page_id = static_cast<PageId>(cursor / capacity);
        u16 slot_in_page = static_cast<u16>(cursor % capacity);

        auto rp = cf.read_page(page_id, page_buf);
        if (rp.is_err())
            return Result<ColumnVector>::err(rp.error().message);

        const ColumnPage view(page_buf);
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
