#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"

#include <memory>
#include <optional>
#include <vector>

namespace nyx {

enum class AggregateKind : u8 {
    COUNT_STAR,
    COUNT,
    SUM,
    AVG,
    MIN,
    MAX,
};

struct AggregateSpec {
    AggregateKind kind;
    std::unique_ptr<Expression> arg;
};

class HashAggregate : public Operator {
  public:
    HashAggregate(std::unique_ptr<Operator> child,
                  std::vector<std::unique_ptr<Expression>> group_keys,
                  std::vector<AggregateSpec> aggregates);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override;
    const Schema& output_schema() const override { return output_schema_; }

  private:
    enum class InternalAggKind : u8 { COUNT_STAR, COUNT, SUM, MIN, MAX };

    struct InternalAgg {
        InternalAggKind kind;
        Expression* arg;
        TypeId input_type;
        TypeId state_type;
    };

    struct OutputBinding {
        enum class Kind : u8 { DIRECT, AVG_DIVIDE };
        Kind kind;
        TypeId output_type;
        u32 state_idx_a;
        u32 state_idx_b;
    };

    struct Entry {
        u32 hash;
        u32 group_idx;
    };

    Result<void> aggregate_all_();
    Chunk emit_slice_();
    void update_state_(size_t agg_idx, u32 group_idx, const std::optional<ColumnVector>& arg_col,
                       u32 arg_row);
    u32 find_or_create_group_(const std::vector<ColumnVector>& key_cols, u32 row);
    void append_initial_state_();
    void resize_table_();

    std::unique_ptr<Operator> child_;
    std::vector<std::unique_ptr<Expression>> group_keys_;
    std::vector<AggregateSpec> aggregates_;
    Schema output_schema_;

    std::vector<InternalAgg> internal_aggs_;
    std::vector<OutputBinding> output_bindings_;

    std::vector<ColumnVector> group_key_cols_;
    std::vector<ColumnVector> group_state_cols_;
    std::vector<Entry> table_;
    u32 mask_ = 0;
    u32 num_groups_ = 0;

    bool opened_ = false;
    bool aggregated_ = false;
    size_t emit_cursor_ = 0;
};

} // namespace nyx
