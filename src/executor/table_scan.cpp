#include "executor/table_scan.h"

#include "storage/disk/column_file.h"
#include "storage/disk/column_page.h"
#include "storage/disk/page.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

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

TableScan::TableScan(Table* table, std::vector<size_t> projected)
    : table_(table), projected_(std::move(projected)) {
    assert(table_ != nullptr);
    output_schema_.reserve(projected_.size());
    for (size_t idx : projected_) {
        assert(idx < table_->column_count());
        output_schema_.push_back(table_->schema()[idx]);
    }
}

Result<void> TableScan::open() {
    total_rows_ = table_->row_count();
    next_row_ = 0;
    opened_ = true;
    return Result<void>::ok();
}

Result<std::optional<Chunk>> TableScan::next() {
    assert(opened_);
    if (next_row_ >= total_rows_)
        return Result<std::optional<Chunk>>::ok(std::nullopt);

    size_t count = static_cast<size_t>(std::min<u64>(CHUNK_SIZE, total_rows_ - next_row_));

    std::vector<ColumnVector> cols;
    cols.reserve(projected_.size());
    for (size_t col_idx : projected_) {
        auto res = read_column_range(col_idx, next_row_, count);
        if (res.is_err())
            return Result<std::optional<Chunk>>::err(res.error().message);
        cols.push_back(std::move(res.value()));
    }

    next_row_ += count;
    return Result<std::optional<Chunk>>::ok(Chunk(count, std::move(cols)));
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
