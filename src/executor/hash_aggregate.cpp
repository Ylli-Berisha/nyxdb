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
    assert(aggregates_.size() == 1);
    assert(aggregates_[0].kind == AggregateKind::COUNT_STAR);

    internal_aggs_.push_back(InternalAgg{
        InternalAggKind::COUNT_STAR,
        nullptr,
        TypeId::INT64,
        TypeId::INT64,
    });
    output_bindings_.push_back(OutputBinding{
        OutputBinding::Kind::DIRECT,
        TypeId::INT64,
        0u,
        0u,
    });

    output_schema_ = {{"count_star", TypeId::INT64, false}};

    group_state_cols_.push_back(ColumnVector::make(TypeId::INT64, 1, false));
    group_state_cols_[0].set_i64(0, 0);
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

        u32 rc = static_cast<u32>(chunk.row_count());
        group_state_cols_[0].set_i64(0, group_state_cols_[0].get_i64(0) + static_cast<i64>(rc));
    }
    aggregated_ = true;
    child_->close();
    return Result<void>::ok();
}

Chunk HashAggregate::emit_slice_() {
    size_t n = std::min<size_t>(TableScan::CHUNK_SIZE, num_groups_ - emit_cursor_);

    std::vector<ColumnVector> out_cols;
    out_cols.reserve(output_bindings_.size());
    for (const auto& b : output_bindings_) {
        ColumnVector out = ColumnVector::empty(b.output_type, false, n);
        const ColumnVector& src = group_state_cols_[b.state_idx_a];
        for (size_t i = 0; i < n; ++i)
            out.append_i64(src.get_i64(emit_cursor_ + i));
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
