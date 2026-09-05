#pragma once

#include "common/result.h"
#include "common/types.h"
#include "executor/chunk.h"
#include "executor/expression.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"

#include <memory>
#include <optional>

namespace nyx {

enum class FilterStrategy : u8 {
    COMPACT,
    SELECTION_VECTOR,
};

class Filter : public Operator {
  public:
    Filter(std::unique_ptr<Operator> child, std::unique_ptr<Expression> predicate,
           FilterStrategy strategy = FilterStrategy::COMPACT);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override;
    const Schema& output_schema() const override { return child_->output_schema(); }

  private:
    std::unique_ptr<Operator> child_;
    std::unique_ptr<Expression> predicate_;
    FilterStrategy strategy_;
    bool opened_ = false;
};

} // namespace nyx
