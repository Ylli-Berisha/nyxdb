#include "executor/index_scan.h"

#include "storage/disk/column_file.h"

#include <numeric>

namespace nyx {

IndexScan::IndexScan(Table* table, BTreeIndex* index,
                     std::vector<size_t> projected,
                     std::optional<Bound> lo,
                     std::optional<Bound> hi)
    : table_(table), index_(index), projected_(std::move(projected)),
      lo_(std::move(lo)), hi_(std::move(hi)) {
    for (size_t ci : projected_) {
        const Column& c = table_->schema()[ci];
        output_schema_.push_back(c);
    }
}

static void append_from_column(ColumnVector& cv, ColumnFile& cf, u64 row_id) {
    if (cf.nullable() && cf.is_null(row_id)) {
        cv.append_null();
        return;
    }
    switch (cf.type()) {
    case TypeId::INT32:     cv.append_i32(cf.get_i32(row_id).value());               break;
    case TypeId::INT64:     cv.append_i64(cf.get_i64(row_id).value());               break;
    case TypeId::DOUBLE:    cv.append_f64(cf.get_f64(row_id).value());               break;
    case TypeId::VARCHAR:   cv.append_str(cf.get_str(row_id));                       break;
    case TypeId::BOOL:      cv.append_bool(cf.get_bool(row_id).value());             break;
    case TypeId::DATE:      cv.append_date(Date{cf.get_date(row_id).value()});       break;
    case TypeId::TIMESTAMP: cv.append_timestamp(Timestamp{cf.get_timestamp(row_id).value()}); break;
    default: break;
    }
}

Result<void> IndexScan::open() {
    const auto& deleted = table_->deleted_bitmap();

    std::vector<byte> lo_key(index_->key_size());
    std::vector<byte> hi_key(index_->key_size());

    const byte* lo_ptr  = nullptr;
    const byte* hi_ptr  = nullptr;
    bool        lo_incl = false;
    bool        hi_incl = false;

    if (lo_) {
        index_->encode_key(lo_->key, lo_key.data());
        lo_ptr  = lo_key.data();
        lo_incl = lo_->inclusive;
    }
    if (hi_) {
        index_->encode_key(hi_->key, hi_key.data());
        hi_ptr  = hi_key.data();
        hi_incl = hi_->inclusive;
    }

    index_->range_scan(lo_ptr, lo_incl, hi_ptr, hi_incl, [&](u64 row_id) -> bool {
        usize byte_idx = row_id / 8;
        u8    bit      = static_cast<u8>(1u << (row_id % 8));
        if (byte_idx < deleted.size() && (deleted[byte_idx] & bit))
            return true;
        row_ids_.push_back(row_id);
        return true;
    });

    return Result<void>::ok();
}

Result<std::optional<Chunk>> IndexScan::next() {
    if (cur_ >= row_ids_.size())
        return Result<std::optional<Chunk>>::ok(std::nullopt);

    size_t count = std::min(CHUNK_SIZE, row_ids_.size() - cur_);

    std::vector<ColumnVector> cols;
    cols.reserve(projected_.size());
    for (size_t ci : projected_) {
        ColumnFile& cf = table_->column(ci);
        cols.push_back(ColumnVector::empty(cf.type(), cf.nullable(), count));
    }

    for (size_t r = 0; r < count; ++r) {
        u64 row_id = row_ids_[cur_ + r];
        for (size_t k = 0; k < projected_.size(); ++k)
            append_from_column(cols[k], table_->column(projected_[k]), row_id);
    }

    cur_ += count;
    return Result<std::optional<Chunk>>::ok(Chunk(count, std::move(cols)));
}

} // namespace nyx
