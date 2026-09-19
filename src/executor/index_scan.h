#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/operator.h"
#include "storage/disk/btree_index.h"
#include "storage/disk/schema.h"
#include "storage/disk/table.h"
#include "storage/disk/value.h"

#include <optional>
#include <vector>

namespace nyx {

class IndexScan : public Operator {
  public:
    struct Bound {
        std::vector<Value> key;
        bool inclusive;
    };

    IndexScan(Table* table, BTreeIndex* index,
              std::vector<size_t> projected,
              std::optional<Bound> lo,
              std::optional<Bound> hi);

    Result<void>               open() override;
    Result<std::optional<Chunk>> next() override;
    void                       close() override {}
    const Schema&              output_schema() const override { return output_schema_; }

  private:
    static constexpr size_t CHUNK_SIZE = 1024;

    Table*       table_;
    BTreeIndex*  index_;
    std::vector<size_t> projected_;
    Schema       output_schema_;

    std::optional<Bound> lo_, hi_;

    std::vector<u64> row_ids_;
    size_t           cur_ = 0;
};

} // namespace nyx
