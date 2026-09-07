#include "executor/hash_aggregate.h"

#include "executor/table_scan.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace nyx {

HashAggregate::HashAggregate(std::unique_ptr<Operator> child,
                             std::vector<std::unique_ptr<Expression>> group_keys,
                             std::vector<AggregateSpec> aggregates)
    : child_(std::move(child)), group_keys_(std::move(group_keys)),
      aggregates_(std::move(aggregates)) {
    assert(child_ != nullptr);
    assert(group_keys_.empty());
    assert(!aggregates_.empty());

    for (const auto& spec : aggregates_) {
        Expression* arg = spec.arg.get();
        InternalAgg ia{};
        bool state_nullable = false;
        const char* name = "agg";

        switch (spec.kind) {
        case AggregateKind::COUNT_STAR:
            ia = {InternalAggKind::COUNT_STAR, nullptr, TypeId::INT64, TypeId::INT64};
            state_nullable = false;
            name = "count_star";
            break;
        case AggregateKind::COUNT: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            ia = {InternalAggKind::COUNT, arg, in, TypeId::INT64};
            state_nullable = false;
            name = "count";
            break;
        }
        case AggregateKind::SUM: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            assert(in == TypeId::INT32 || in == TypeId::INT64 || in == TypeId::DOUBLE);
            TypeId state = (in == TypeId::INT32) ? TypeId::INT64 : in;
            ia = {InternalAggKind::SUM, arg, in, state};
            state_nullable = true;
            name = "sum";
            break;
        }
        case AggregateKind::MIN: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            assert(in == TypeId::INT32 || in == TypeId::INT64 || in == TypeId::DOUBLE);
            ia = {InternalAggKind::MIN, arg, in, in};
            state_nullable = true;
            name = "min";
            break;
        }
        case AggregateKind::MAX: {
            assert(arg != nullptr);
            TypeId in = arg->output_type();
            assert(in == TypeId::INT32 || in == TypeId::INT64 || in == TypeId::DOUBLE);
            ia = {InternalAggKind::MAX, arg, in, in};
            state_nullable = true;
            name = "max";
            break;
        }
        case AggregateKind::AVG:
            assert(false && "AVG lands in commit 3");
            break;
        }

        u32 state_idx = static_cast<u32>(internal_aggs_.size());
        internal_aggs_.push_back(ia);

        ColumnVector col = ColumnVector::make(ia.state_type, 1, state_nullable);
        if (state_nullable) {
            col.set_null(0);
        } else {
            col.set_i64(0, 0);
        }
        group_state_cols_.push_back(std::move(col));

        output_bindings_.push_back(
            OutputBinding{OutputBinding::Kind::DIRECT, ia.state_type, state_idx, 0u});
        output_schema_.push_back({name, ia.state_type, state_nullable});
    }

    num_groups_ = 1;
}

Result<void> HashAggregate::open() {
    auto r = child_->open();
    if (r.is_err())
        return r;
    opened_ = true;
    return Result<void>::ok();
}

void HashAggregate::close() {
    if (child_)
        child_->close();
}

void HashAggregate::update_state_(size_t agg_idx, u32 group_idx,
                                  const std::optional<ColumnVector>& arg_col, u32 arg_row) {
    const InternalAgg& ia = internal_aggs_[agg_idx];
    ColumnVector& state = group_state_cols_[agg_idx];

    switch (ia.kind) {
    case InternalAggKind::COUNT_STAR:
        state.set_i64(group_idx, state.get_i64(group_idx) + 1);
        return;
    case InternalAggKind::COUNT:
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        state.set_i64(group_idx, state.get_i64(group_idx) + 1);
        return;
    case InternalAggKind::SUM: {
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        switch (ia.state_type) {
        case TypeId::INT64: {
            i64 add = (ia.input_type == TypeId::INT32) ? static_cast<i64>(arg_col->get_i32(arg_row))
                                                       : arg_col->get_i64(arg_row);
            i64 cur = state.is_null(group_idx) ? 0 : state.get_i64(group_idx);
            state.set_i64(group_idx, cur + add);
            return;
        }
        case TypeId::DOUBLE: {
            f64 add = arg_col->get_f64(arg_row);
            f64 cur = state.is_null(group_idx) ? 0.0 : state.get_f64(group_idx);
            state.set_f64(group_idx, cur + add);
            return;
        }
        default:
            assert(false);
            return;
        }
    }
    case InternalAggKind::MIN: {
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        switch (ia.state_type) {
        case TypeId::INT32: {
            i32 v = arg_col->get_i32(arg_row);
            if (state.is_null(group_idx) || v < state.get_i32(group_idx))
                state.set_i32(group_idx, v);
            return;
        }
        case TypeId::INT64: {
            i64 v = arg_col->get_i64(arg_row);
            if (state.is_null(group_idx) || v < state.get_i64(group_idx))
                state.set_i64(group_idx, v);
            return;
        }
        case TypeId::DOUBLE: {
            f64 v = arg_col->get_f64(arg_row);
            if (state.is_null(group_idx) || v < state.get_f64(group_idx))
                state.set_f64(group_idx, v);
            return;
        }
        default:
            assert(false);
            return;
        }
    }
    case InternalAggKind::MAX: {
        assert(arg_col.has_value());
        if (arg_col->is_null(arg_row))
            return;
        switch (ia.state_type) {
        case TypeId::INT32: {
            i32 v = arg_col->get_i32(arg_row);
            if (state.is_null(group_idx) || v > state.get_i32(group_idx))
                state.set_i32(group_idx, v);
            return;
        }
        case TypeId::INT64: {
            i64 v = arg_col->get_i64(arg_row);
            if (state.is_null(group_idx) || v > state.get_i64(group_idx))
                state.set_i64(group_idx, v);
            return;
        }
        case TypeId::DOUBLE: {
            f64 v = arg_col->get_f64(arg_row);
            if (state.is_null(group_idx) || v > state.get_f64(group_idx))
                state.set_f64(group_idx, v);
            return;
        }
        default:
            assert(false);
            return;
        }
    }
    }
}

Result<void> HashAggregate::aggregate_all_() {
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

        std::vector<std::optional<ColumnVector>> arg_cols;
        arg_cols.reserve(internal_aggs_.size());
        for (auto& ia : internal_aggs_) {
            if (ia.arg) {
                auto ev = ia.arg->evaluate(chunk);
                if (ev.is_err())
                    return Result<void>::err(ev.error().message);
                arg_cols.push_back(std::move(ev.value()));
            } else {
                arg_cols.push_back(std::nullopt);
            }
        }

        u32 rc = static_cast<u32>(chunk.row_count());
        for (u32 r = 0; r < rc; ++r) {
            for (size_t a = 0; a < internal_aggs_.size(); ++a)
                update_state_(a, 0u, arg_cols[a], r);
        }
    }
    aggregated_ = true;
    child_->close();
    return Result<void>::ok();
}

Chunk HashAggregate::emit_slice_() {
    size_t n = std::min<size_t>(TableScan::CHUNK_SIZE, num_groups_ - emit_cursor_);

    std::vector<ColumnVector> out_cols;
    out_cols.reserve(output_bindings_.size());
    for (size_t c = 0; c < output_bindings_.size(); ++c) {
        const OutputBinding& b = output_bindings_[c];
        const ColumnVector& src = group_state_cols_[b.state_idx_a];
        bool nullable = output_schema_[c].nullable;
        ColumnVector out = ColumnVector::empty(b.output_type, nullable, n);
        switch (b.output_type) {
        case TypeId::INT32:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (nullable && src.is_null(g))
                    out.append_null();
                else
                    out.append_i32(src.get_i32(g));
            }
            break;
        case TypeId::INT64:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (nullable && src.is_null(g))
                    out.append_null();
                else
                    out.append_i64(src.get_i64(g));
            }
            break;
        case TypeId::DOUBLE:
            for (size_t i = 0; i < n; ++i) {
                size_t g = emit_cursor_ + i;
                if (nullable && src.is_null(g))
                    out.append_null();
                else
                    out.append_f64(src.get_f64(g));
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

Result<std::optional<Chunk>> HashAggregate::next() {
    assert(opened_);
    if (!aggregated_) {
        auto r = aggregate_all_();
        if (r.is_err())
            return Result<std::optional<Chunk>>::err(r.error().message);
    }
    if (emit_cursor_ >= num_groups_)
        return Result<std::optional<Chunk>>::ok(std::nullopt);
    return Result<std::optional<Chunk>>::ok(emit_slice_());
}

} // namespace nyx
