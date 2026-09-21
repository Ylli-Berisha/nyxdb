#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"
#include "storage/disk/segment.h"
#include "storage/disk/table.h"
#include "storage/disk/value.h"

#include <cstddef>
#include <optional>
#include <shared_mutex>
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
    void close() override;
    const Schema& output_schema() const override { return output_schema_; }
    u64 last_chunk_physical_start() const { return last_chunk_physical_start_; }

  private:
    struct SegScanState {
        Segment* seg; // null = write buffer
        u64 base_row_id;
        u64 local_row_count;
        std::vector<std::pair<u64, u64>> survivors; // local row ID ranges
        usize range_idx = 0;
        u64 cur_local = 0;
    };

    Result<void> build_entry(SegScanState& st);
    Result<ColumnVector> read_col(SegScanState& st, size_t col_idx, u64 local_start, size_t count);

    Table* table_;
    std::vector<size_t> projected_;
    std::optional<ScanRange> range_;
    Schema output_schema_;

    std::shared_lock<std::shared_mutex> lock_;
    std::vector<SegScanState> scan_plan_;
    usize current_seg_ = 0;
    u64 last_chunk_physical_start_ = 0;
    bool opened_ = false;
};

} // namespace nyx
