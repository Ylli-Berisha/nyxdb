#pragma once

#include "common/result.h"
#include "executor/chunk.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"

#include <cstddef>
#include <memory>
#include <optional>

namespace nyx {

class Limit : public Operator {
  public:
    Limit(std::unique_ptr<Operator> child, size_t limit);
    Limit(std::unique_ptr<Operator> child, size_t offset, size_t limit);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override;
    const Schema& output_schema() const override { return child_->output_schema(); }

  private:
    void close_child_eager();

    std::unique_ptr<Operator> child_;
    size_t offset_;
    size_t limit_;
    size_t skipped_ = 0;
    size_t emitted_ = 0;
    bool opened_ = false;
    bool exhausted_ = false;
};

} // namespace nyx
