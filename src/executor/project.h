#pragma once

#include "common/result.h"
#include "executor/chunk.h"
#include "executor/expression.h"
#include "executor/operator.h"
#include "storage/disk/schema.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

struct ProjectItem {
    std::unique_ptr<Expression> expr;
    std::string name;
    bool nullable;
};

class Project : public Operator {
  public:
    Project(std::unique_ptr<Operator> child, std::vector<ProjectItem> items);

    Result<void> open() override;
    Result<std::optional<Chunk>> next() override;
    void close() override;
    const Schema& output_schema() const override { return output_schema_; }

  private:
    std::unique_ptr<Operator> child_;
    std::vector<ProjectItem> items_;
    Schema output_schema_;
    bool opened_ = false;
};

} // namespace nyx
