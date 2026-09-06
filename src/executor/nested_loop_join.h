#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/hash_join.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"

#include <memory>
#include <optional>
#include <vector>

namespace nyx {

class NestedLoopJoin : public Operator {
  public:
    NestedLoopJoin(std::unique_ptr<Operator> outer, std::unique_ptr<Operator> inner,
                   std::unique_ptr<Expression> predicate, JoinType type = JoinType::INNER);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override;
    const Schema& output_schema() const override { return output_schema_; }

  private:
    struct MatchPair {
        u32 outer_chunk;
        u32 outer_row;
        u32 inner_row;
    };

    Result<void> buffer_outer_();
    Result<bool> load_next_inner_chunk_();
    Chunk emit_output_slice_();

    std::unique_ptr<Operator> outer_child_;
    std::unique_ptr<Operator> inner_child_;
    std::unique_ptr<Expression> predicate_;
    JoinType type_;
    Schema output_schema_;
    size_t n_outer_cols_ = 0;
    size_t n_inner_cols_ = 0;

    bool opened_ = false;
    bool buffered_ = false;
    bool inner_exhausted_ = false;

    std::vector<Chunk> outer_chunks_;

    std::optional<Chunk> inner_chunk_;
    std::vector<MatchPair> match_pairs_;
    size_t emit_cursor_ = 0;
};

} // namespace nyx
