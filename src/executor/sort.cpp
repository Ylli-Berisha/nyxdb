#include "executor/sort.h"

#include "executor/table_scan.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace nyx {

Sort::Sort(std::unique_ptr<Operator> child, std::vector<SortKey> keys)
    : child_(std::move(child)), keys_(std::move(keys)) {
    assert(child_ != nullptr);
    assert(!keys_.empty());
    for (const auto& k : keys_) {
        (void)k;
        assert(k.expr != nullptr);
    }
}

Result<void> Sort::open() {
    auto r = child_->open();
    if (r.is_err())
        return r;
    opened_ = true;
    return Result<void>::ok();
}

void Sort::close() {
    if (child_)
        child_->close();
}

Result<void> Sort::consume_and_sort_() {
    while (true) {
        auto in = child_->next();
        if (in.is_err())
            return Result<void>::err(in.error().message);
        if (!in.value().has_value())
            break;

        Chunk chunk = std::move(*in.value());
        if (chunk.logical_size() == 0)
            continue;
        chunk.materialize();

        std::vector<ColumnVector> per_chunk_keys;
        per_chunk_keys.reserve(keys_.size());
        for (auto& k : keys_) {
            auto ev = k.expr->evaluate(chunk);
            if (ev.is_err())
                return Result<void>::err(ev.error().message);
            per_chunk_keys.push_back(std::move(ev.value()));
        }

        buffered_.push_back(std::move(chunk));
        key_cols_.push_back(std::move(per_chunk_keys));
    }
    child_->close();

    size_t total = 0;
    for (const auto& c : buffered_)
        total += c.row_count();
    perm_.reserve(total);
    for (u32 ci = 0; ci < buffered_.size(); ++ci) {
        u32 rc = static_cast<u32>(buffered_[ci].row_count());
        for (u32 r = 0; r < rc; ++r)
            perm_.push_back(RowRef{ci, r});
    }

    std::stable_sort(perm_.begin(), perm_.end(),
                     [this](const RowRef& l, const RowRef& r) { return less_(l, r); });

    consumed_ = true;
    return Result<void>::ok();
}

static int compare_typed(const ColumnVector& lc, size_t li, const ColumnVector& rc, size_t ri) {
    assert(lc.type() == rc.type());
    switch (lc.type()) {
    case TypeId::INT32: {
        i32 a = lc.get_i32(li);
        i32 b = rc.get_i32(ri);
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    case TypeId::INT64: {
        i64 a = lc.get_i64(li);
        i64 b = rc.get_i64(ri);
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    case TypeId::DOUBLE: {
        f64 a = lc.get_f64(li);
        f64 b = rc.get_f64(ri);
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    default:
        assert(false);
        return 0;
    }
}

bool Sort::less_(const RowRef& l, const RowRef& r) const {
    for (size_t k = 0; k < keys_.size(); ++k) {
        const ColumnVector& lc = key_cols_[l.chunk_idx][k];
        const ColumnVector& rc = key_cols_[r.chunk_idx][k];
        bool ln = lc.is_null(l.row_idx);
        bool rn = rc.is_null(r.row_idx);
        if (ln && rn)
            continue;
        if (ln)
            return keys_[k].nulls == NullOrder::FIRST;
        if (rn)
            return keys_[k].nulls == NullOrder::LAST;
        int cmp = compare_typed(lc, l.row_idx, rc, r.row_idx);
        if (cmp == 0)
            continue;
        return keys_[k].direction == SortDirection::ASC ? (cmp < 0) : (cmp > 0);
    }
    return false;
}

Chunk Sort::emit_slice_(size_t begin, size_t end) {
    assert(begin < end);
    assert(!buffered_.empty());
    const Chunk& proto = buffered_.front();
    size_t ncols = proto.column_count();
    size_t n = end - begin;

    std::vector<ColumnVector> out_cols;
    out_cols.reserve(ncols);
    for (size_t c = 0; c < ncols; ++c) {
        TypeId t = proto.column(c).type();
        bool nullable = proto.column(c).nullable();
        ColumnVector col = ColumnVector::empty(t, nullable, n);
        switch (t) {
        case TypeId::INT32:
            for (size_t i = begin; i < end; ++i) {
                const RowRef& rr = perm_[i];
                const ColumnVector& src = buffered_[rr.chunk_idx].column(c);
                if (nullable && src.is_null(rr.row_idx))
                    col.append_null();
                else
                    col.append_i32(src.get_i32(rr.row_idx));
            }
            break;
        case TypeId::INT64:
            for (size_t i = begin; i < end; ++i) {
                const RowRef& rr = perm_[i];
                const ColumnVector& src = buffered_[rr.chunk_idx].column(c);
                if (nullable && src.is_null(rr.row_idx))
                    col.append_null();
                else
                    col.append_i64(src.get_i64(rr.row_idx));
            }
            break;
        case TypeId::DOUBLE:
            for (size_t i = begin; i < end; ++i) {
                const RowRef& rr = perm_[i];
                const ColumnVector& src = buffered_[rr.chunk_idx].column(c);
                if (nullable && src.is_null(rr.row_idx))
                    col.append_null();
                else
                    col.append_f64(src.get_f64(rr.row_idx));
            }
            break;
        default:
            assert(false);
        }
        out_cols.push_back(std::move(col));
    }
    return Chunk(n, std::move(out_cols));
}

Result<std::optional<Chunk>> Sort::next() {
    assert(opened_);
    if (!consumed_) {
        auto r = consume_and_sort_();
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
    }
    if (emit_cursor_ >= perm_.size())
        return Result<std::optional<Chunk>>::ok(std::nullopt);

    size_t end = std::min(emit_cursor_ + TableScan::CHUNK_SIZE, perm_.size());
    Chunk out = emit_slice_(emit_cursor_, end);
    emit_cursor_ = end;
    return Result<std::optional<Chunk>>::ok(std::move(out));
}

} // namespace nyx
