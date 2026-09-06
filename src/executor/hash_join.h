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

enum class JoinType : u8 {
    INNER,
    LEFT_OUTER,
    SEMI,
    ANTI,
};

class HashJoin : public Operator {
  public:
    HashJoin(std::unique_ptr<Operator> build, std::unique_ptr<Operator> probe,
             std::vector<std::unique_ptr<Expression>> build_keys,
             std::vector<std::unique_ptr<Expression>> probe_keys, JoinType type = JoinType::INNER);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override;
    const Schema& output_schema() const override { return output_schema_; }

  private:
    struct Entry {
        u32 hash;
        u32 chunk_idx;
        u32 row_idx;
    };
    struct MatchPair {
        u32 probe_row;
        u32 build_chunk;
        u32 build_row;
    };

    Result<void> build_side_();
    Result<bool> load_next_probe_chunk_();
    Chunk emit_output_slice_();

    std::unique_ptr<Operator> build_child_;
    std::unique_ptr<Operator> probe_child_;
    std::vector<std::unique_ptr<Expression>> build_keys_;
    std::vector<std::unique_ptr<Expression>> probe_keys_;
    JoinType type_;
    Schema output_schema_;
    size_t n_probe_cols_ = 0;
    size_t n_build_cols_ = 0;

    bool opened_ = false;
    bool built_ = false;
    bool probe_exhausted_ = false;

    std::vector<Chunk> build_chunks_;
    std::vector<std::vector<ColumnVector>> build_key_cols_;
    std::vector<Entry> table_;
    u32 mask_ = 0;

    std::optional<Chunk> probe_chunk_;
    std::vector<ColumnVector> probe_key_cols_;
    std::vector<MatchPair> match_pairs_;
    size_t emit_cursor_ = 0;
};

} // namespace nyx
