#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"
#include "storage/disk/table.h"
#include "storage/disk/value.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace nyx {

struct ScanRange {
    size_t col_idx;
    std::optional<Value> lo;
    std::optional<Value> hi;
};

class TableScan : public Operator {
  public:
    static constexpr size_t CHUNK_SIZE = 1024;

    TableScan(Table* table, std::vector<size_t> projected);
    TableScan(Table* table, std::vector<size_t> projected, ScanRange range);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override {}
    const Schema& output_schema() const override { return output_schema_; }

  private:
    Result<ColumnVector> read_column_range(size_t col_idx, u64 start, size_t count);
    Result<std::vector<ColumnVector>> read_projected_columns(u64 start, size_t count);
    Result<void> compute_survivors();
    Result<std::optional<Chunk>> next_no_range();
    Result<std::optional<Chunk>> next_with_survivors();

    Table* table_;
    std::vector<size_t> projected_;
    std::optional<ScanRange> range_;
    Schema output_schema_;

    u64 next_row_ = 0;
    u64 total_rows_ = 0;

    std::vector<std::pair<u64, u64>> survivors_;
    size_t cur_range_ = 0;
    u64 cur_row_ = 0;

    bool opened_ = false;
};

} // namespace nyx
