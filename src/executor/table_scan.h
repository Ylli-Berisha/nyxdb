#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"
#include "storage/disk/table.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace nyx {

class TableScan : public Operator {
  public:
    static constexpr size_t CHUNK_SIZE = 1024;

    TableScan(Table* table, std::vector<size_t> projected);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override {}
    const Schema& output_schema() const override { return output_schema_; }

  private:
    Result<ColumnVector> read_column_range(size_t col_idx, u64 start, size_t count);

    Table* table_;
    std::vector<size_t> projected_;
    Schema output_schema_;
    u64 next_row_ = 0;
    u64 total_rows_ = 0;
    bool opened_ = false;
};

} // namespace nyx
