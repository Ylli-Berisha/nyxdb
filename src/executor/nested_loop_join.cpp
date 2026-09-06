#include "executor/nested_loop_join.h"

#include "executor/table_scan.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace nyx {

static ColumnVector broadcast_value(const ColumnVector& src, size_t src_row, size_t out_size) {
    ColumnVector out = ColumnVector::make(src.type(), out_size, src.nullable());
    if (src.nullable() && src.is_null(src_row)) {
        for (size_t i = 0; i < out_size; ++i)
            out.set_null(i);
        return out;
    }
    switch (src.type()) {
    case TypeId::INT32: {
        i32 v = src.get_i32(src_row);
        for (size_t i = 0; i < out_size; ++i)
            out.set_i32(i, v);
        break;
    }
    case TypeId::INT64: {
        i64 v = src.get_i64(src_row);
        for (size_t i = 0; i < out_size; ++i)
            out.set_i64(i, v);
        break;
    }
    case TypeId::DOUBLE: {
        f64 v = src.get_f64(src_row);
        for (size_t i = 0; i < out_size; ++i)
            out.set_f64(i, v);
        break;
    }
    default:
        assert(false);
    }
    return out;
}

NestedLoopJoin::NestedLoopJoin(std::unique_ptr<Operator> outer, std::unique_ptr<Operator> inner,
                               std::unique_ptr<Expression> predicate, JoinType type)
    : outer_child_(std::move(outer)), inner_child_(std::move(inner)),
      predicate_(std::move(predicate)), type_(type) {
    assert(outer_child_ != nullptr);
    assert(inner_child_ != nullptr);
    assert(predicate_ != nullptr);
    assert(predicate_->output_type() == TypeId::INT32);
    assert(type_ == JoinType::INNER);

    output_schema_ = outer_child_->output_schema();
    for (const auto& col : inner_child_->output_schema())
        output_schema_.push_back(col);
    n_outer_cols_ = outer_child_->output_schema().size();
    n_inner_cols_ = inner_child_->output_schema().size();
}

Result<void> NestedLoopJoin::open() {
    auto ro = outer_child_->open();
    if (ro.is_err())
        return ro;
    auto ri = inner_child_->open();
    if (ri.is_err())
        return ri;
    opened_ = true;
    return Result<void>::ok();
}

void NestedLoopJoin::close() {
    if (outer_child_)
        outer_child_->close();
    if (inner_child_)
        inner_child_->close();
}

Result<void> NestedLoopJoin::buffer_outer_() {
    while (true) {
        auto in = outer_child_->next();
        if (in.is_err())
            return Result<void>::err(in.error().message);
        if (!in.value().has_value())
            break;
        Chunk chunk = std::move(*in.value());
        if (chunk.logical_size() == 0)
            continue;
        chunk.materialize();
        outer_chunks_.push_back(std::move(chunk));
    }
    buffered_ = true;
    outer_child_->close();
    return Result<void>::ok();
}

Result<bool> NestedLoopJoin::load_next_inner_chunk_() {
    while (true) {
        auto in = inner_child_->next();
        if (in.is_err())
            return Result<bool>::err(in.error().message);
        if (!in.value().has_value()) {
            inner_exhausted_ = true;
            return Result<bool>::ok(false);
        }
        Chunk chunk = std::move(*in.value());
        if (chunk.logical_size() == 0)
            continue;
        chunk.materialize();

        size_t n_inner = chunk.row_count();
        match_pairs_.clear();
        for (u32 oc = 0; oc < static_cast<u32>(outer_chunks_.size()); ++oc) {
            const Chunk& outer_chunk = outer_chunks_[oc];
            u32 orc = static_cast<u32>(outer_chunk.row_count());
            for (u32 orow = 0; orow < orc; ++orow) {
                std::vector<ColumnVector> combined_cols;
                combined_cols.reserve(n_outer_cols_ + n_inner_cols_);
                for (size_t c = 0; c < n_outer_cols_; ++c)
                    combined_cols.push_back(broadcast_value(outer_chunk.column(c), orow, n_inner));
                for (size_t c = 0; c < n_inner_cols_; ++c)
                    combined_cols.push_back(chunk.column(c));
                Chunk combined(n_inner, std::move(combined_cols));

                auto ev = predicate_->evaluate(combined);
                if (ev.is_err())
                    return Result<bool>::err(ev.error().message);
                const ColumnVector& mask = ev.value();
                for (u32 i = 0; i < static_cast<u32>(n_inner); ++i) {
                    if (!mask.is_null(i) && mask.get_i32(i) != 0)
                        match_pairs_.push_back(MatchPair{oc, orow, i});
                }
            }
        }
        emit_cursor_ = 0;
        inner_chunk_ = std::move(chunk);
        return Result<bool>::ok(true);
    }
}

Chunk NestedLoopJoin::emit_output_slice_() {
    assert(inner_chunk_.has_value());
    size_t remaining = match_pairs_.size() - emit_cursor_;
    size_t n = std::min(static_cast<size_t>(TableScan::CHUNK_SIZE), remaining);

    std::vector<ColumnVector> out_cols;
    out_cols.reserve(output_schema_.size());

    for (size_t c = 0; c < output_schema_.size(); ++c) {
        bool is_outer = c < n_outer_cols_;
        size_t src_col_idx = is_outer ? c : c - n_outer_cols_;
        TypeId t = output_schema_[c].type;
        bool nullable = output_schema_[c].nullable;
        ColumnVector out = ColumnVector::empty(t, nullable, n);

        switch (t) {
        case TypeId::INT32:
            for (size_t i = 0; i < n; ++i) {
                const MatchPair& mp = match_pairs_[emit_cursor_ + i];
                const ColumnVector& src = is_outer
                                              ? outer_chunks_[mp.outer_chunk].column(src_col_idx)
                                              : inner_chunk_->column(src_col_idx);
                size_t row = is_outer ? mp.outer_row : mp.inner_row;
                if (nullable && src.is_null(row))
                    out.append_null();
                else
                    out.append_i32(src.get_i32(row));
            }
            break;
        case TypeId::INT64:
            for (size_t i = 0; i < n; ++i) {
                const MatchPair& mp = match_pairs_[emit_cursor_ + i];
                const ColumnVector& src = is_outer
                                              ? outer_chunks_[mp.outer_chunk].column(src_col_idx)
                                              : inner_chunk_->column(src_col_idx);
                size_t row = is_outer ? mp.outer_row : mp.inner_row;
                if (nullable && src.is_null(row))
                    out.append_null();
                else
                    out.append_i64(src.get_i64(row));
            }
            break;
        case TypeId::DOUBLE:
            for (size_t i = 0; i < n; ++i) {
                const MatchPair& mp = match_pairs_[emit_cursor_ + i];
                const ColumnVector& src = is_outer
                                              ? outer_chunks_[mp.outer_chunk].column(src_col_idx)
                                              : inner_chunk_->column(src_col_idx);
                size_t row = is_outer ? mp.outer_row : mp.inner_row;
                if (nullable && src.is_null(row))
                    out.append_null();
                else
                    out.append_f64(src.get_f64(row));
            }
            break;
        default:
            assert(false);
        }
        out_cols.push_back(std::move(out));
    }
    emit_cursor_ += n;
    return Chunk(n, std::move(out_cols));
}

Result<std::optional<Chunk>> NestedLoopJoin::next() {
    assert(opened_);
    if (!buffered_) {
        auto r = buffer_outer_();
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
    }
    while (true) {
        if (emit_cursor_ < match_pairs_.size())
            return Result<std::optional<Chunk>>::ok(emit_output_slice_());
        if (inner_exhausted_)
            return Result<std::optional<Chunk>>::ok(std::nullopt);
        auto r = load_next_inner_chunk_();
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
    }
}

} // namespace nyx
