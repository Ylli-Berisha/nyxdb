#include "executor/hash_join.h"

#include "common/xxhash.h"
#include "executor/table_scan.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <utility>

namespace nyx {

static u32 next_pow2(u32 x) {
    if (x <= 1)
        return 1;
    return 1u << (32 - __builtin_clz(x - 1));
}

static u32 hash_i64(i64 v) {
    return static_cast<u32>(xxhash64(reinterpret_cast<const byte*>(&v), sizeof(i64)));
}

HashJoin::HashJoin(std::unique_ptr<Operator> build, std::unique_ptr<Operator> probe,
                   std::vector<std::unique_ptr<Expression>> build_keys,
                   std::vector<std::unique_ptr<Expression>> probe_keys, JoinType type)
    : build_child_(std::move(build)), probe_child_(std::move(probe)),
      build_keys_(std::move(build_keys)), probe_keys_(std::move(probe_keys)), type_(type) {
    assert(build_child_ != nullptr);
    assert(probe_child_ != nullptr);
    assert(!build_keys_.empty());
    assert(build_keys_.size() == probe_keys_.size());
    assert(build_keys_.size() == 1);
    assert(build_keys_[0]->output_type() == TypeId::INT64);
    assert(probe_keys_[0]->output_type() == TypeId::INT64);
    assert(type_ == JoinType::INNER);

    output_schema_ = probe_child_->output_schema();
    for (const auto& col : build_child_->output_schema())
        output_schema_.push_back(col);
    n_probe_cols_ = probe_child_->output_schema().size();
    n_build_cols_ = build_child_->output_schema().size();
}

Result<void> HashJoin::open() {
    auto rb = build_child_->open();
    if (rb.is_err())
        return rb;
    auto rp = probe_child_->open();
    if (rp.is_err())
        return rp;
    opened_ = true;
    return Result<void>::ok();
}

void HashJoin::close() {
    if (build_child_)
        build_child_->close();
    if (probe_child_)
        probe_child_->close();
}

Result<void> HashJoin::build_side_() {
    while (true) {
        auto in = build_child_->next();
        if (in.is_err())
            return Result<void>::err(in.error().message);
        if (!in.value().has_value())
            break;

        Chunk chunk = std::move(*in.value());
        if (chunk.logical_size() == 0)
            continue;
        chunk.materialize();

        std::vector<ColumnVector> per_chunk_keys;
        per_chunk_keys.reserve(build_keys_.size());
        for (auto& k : build_keys_) {
            auto ev = k->evaluate(chunk);
            if (ev.is_err())
                return Result<void>::err(ev.error().message);
            per_chunk_keys.push_back(std::move(ev.value()));
        }

        build_chunks_.push_back(std::move(chunk));
        build_key_cols_.push_back(std::move(per_chunk_keys));
    }

    size_t n_nonnull = 0;
    for (size_t ci = 0; ci < build_chunks_.size(); ++ci) {
        const ColumnVector& key = build_key_cols_[ci][0];
        for (size_t r = 0; r < build_chunks_[ci].row_count(); ++r) {
            if (!key.is_null(r))
                ++n_nonnull;
        }
    }

    u32 table_size = next_pow2(std::max<u32>(1u, static_cast<u32>(n_nonnull) * 2u));
    table_.assign(table_size, Entry{0u, UINT32_MAX, 0u});
    mask_ = table_size - 1;

    for (u32 ci = 0; ci < static_cast<u32>(build_chunks_.size()); ++ci) {
        const ColumnVector& key = build_key_cols_[ci][0];
        u32 rc = static_cast<u32>(build_chunks_[ci].row_count());
        for (u32 r = 0; r < rc; ++r) {
            if (key.is_null(r))
                continue;
            u32 h = hash_i64(key.get_i64(r));
            u32 slot = h & mask_;
            while (table_[slot].chunk_idx != UINT32_MAX)
                slot = (slot + 1u) & mask_;
            table_[slot] = Entry{h, ci, r};
        }
    }

    built_ = true;
    build_child_->close();
    return Result<void>::ok();
}

Result<bool> HashJoin::load_next_probe_chunk_() {
    while (true) {
        auto in = probe_child_->next();
        if (in.is_err())
            return Result<bool>::err(in.error().message);
        if (!in.value().has_value()) {
            probe_exhausted_ = true;
            return Result<bool>::ok(false);
        }

        Chunk chunk = std::move(*in.value());
        if (chunk.logical_size() == 0)
            continue;
        chunk.materialize();

        std::vector<ColumnVector> per_chunk_keys;
        per_chunk_keys.reserve(probe_keys_.size());
        for (auto& k : probe_keys_) {
            auto ev = k->evaluate(chunk);
            if (ev.is_err())
                return Result<bool>::err(ev.error().message);
            per_chunk_keys.push_back(std::move(ev.value()));
        }

        match_pairs_.clear();
        const ColumnVector& probe_key = per_chunk_keys[0];
        u32 rc = static_cast<u32>(chunk.row_count());
        for (u32 r = 0; r < rc; ++r) {
            if (probe_key.is_null(r))
                continue;
            i64 pk = probe_key.get_i64(r);
            u32 h = hash_i64(pk);
            u32 slot = h & mask_;
            while (table_[slot].chunk_idx != UINT32_MAX) {
                if (table_[slot].hash == h) {
                    const Entry& e = table_[slot];
                    const ColumnVector& bk = build_key_cols_[e.chunk_idx][0];
                    if (bk.get_i64(e.row_idx) == pk)
                        match_pairs_.push_back(MatchPair{r, e.chunk_idx, e.row_idx});
                }
                slot = (slot + 1u) & mask_;
            }
        }
        emit_cursor_ = 0;

        probe_chunk_ = std::move(chunk);
        probe_key_cols_ = std::move(per_chunk_keys);
        return Result<bool>::ok(true);
    }
}

Chunk HashJoin::emit_output_slice_() {
    assert(probe_chunk_.has_value());
    size_t remaining = match_pairs_.size() - emit_cursor_;
    size_t n = std::min(static_cast<size_t>(TableScan::CHUNK_SIZE), remaining);

    std::vector<ColumnVector> out_cols;
    out_cols.reserve(output_schema_.size());

    for (size_t c = 0; c < output_schema_.size(); ++c) {
        bool is_probe = c < n_probe_cols_;
        size_t src_col_idx = is_probe ? c : c - n_probe_cols_;
        TypeId t = output_schema_[c].type;
        bool nullable = output_schema_[c].nullable;
        ColumnVector out = ColumnVector::empty(t, nullable, n);

        switch (t) {
        case TypeId::INT32:
            for (size_t i = 0; i < n; ++i) {
                const MatchPair& mp = match_pairs_[emit_cursor_ + i];
                const ColumnVector& src = is_probe
                                              ? probe_chunk_->column(src_col_idx)
                                              : build_chunks_[mp.build_chunk].column(src_col_idx);
                size_t row = is_probe ? mp.probe_row : mp.build_row;
                if (nullable && src.is_null(row))
                    out.append_null();
                else
                    out.append_i32(src.get_i32(row));
            }
            break;
        case TypeId::INT64:
            for (size_t i = 0; i < n; ++i) {
                const MatchPair& mp = match_pairs_[emit_cursor_ + i];
                const ColumnVector& src = is_probe
                                              ? probe_chunk_->column(src_col_idx)
                                              : build_chunks_[mp.build_chunk].column(src_col_idx);
                size_t row = is_probe ? mp.probe_row : mp.build_row;
                if (nullable && src.is_null(row))
                    out.append_null();
                else
                    out.append_i64(src.get_i64(row));
            }
            break;
        case TypeId::DOUBLE:
            for (size_t i = 0; i < n; ++i) {
                const MatchPair& mp = match_pairs_[emit_cursor_ + i];
                const ColumnVector& src = is_probe
                                              ? probe_chunk_->column(src_col_idx)
                                              : build_chunks_[mp.build_chunk].column(src_col_idx);
                size_t row = is_probe ? mp.probe_row : mp.build_row;
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

Result<std::optional<Chunk>> HashJoin::next() {
    assert(opened_);
    if (!built_) {
        auto r = build_side_();
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
    }
    while (true) {
        if (emit_cursor_ < match_pairs_.size())
            return Result<std::optional<Chunk>>::ok(emit_output_slice_());
        if (probe_exhausted_)
            return Result<std::optional<Chunk>>::ok(std::nullopt);
        auto r = load_next_probe_chunk_();
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
    }
}

} // namespace nyx
