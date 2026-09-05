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

enum class SortDirection : u8 {
    ASC,
    DESC,
};

enum class NullOrder : u8 {
    FIRST,
    LAST,
};

struct SortKey {
    std::unique_ptr<Expression> expr;
    SortDirection direction = SortDirection::ASC;
    NullOrder nulls = NullOrder::LAST;
};

class Sort : public Operator {
  public:
    Sort(std::unique_ptr<Operator> child, std::vector<SortKey> keys);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override;
    const Schema& output_schema() const override { return child_->output_schema(); }

  private:
    struct RowRef {
        u32 chunk_idx;
        u32 row_idx;
    };

    Result<void> consume_and_sort_();
    Chunk emit_slice_(size_t begin, size_t end);
    bool less_(const RowRef& l, const RowRef& r) const;

    std::unique_ptr<Operator> child_;
    std::vector<SortKey> keys_;
    bool opened_ = false;
    bool consumed_ = false;
    std::vector<Chunk> buffered_;
    std::vector<std::vector<ColumnVector>> key_cols_;
    std::vector<RowRef> perm_;
    size_t emit_cursor_ = 0;
};

} // namespace nyx
