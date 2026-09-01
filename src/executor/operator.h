#pragma once

#include "common/result.h"
#include "executor/chunk.h"
#include "storage/disk/schema.h"

#include <optional>

namespace nyx {

class Operator {
  public:
    virtual ~Operator() = default;

    virtual Result<void> open() { return Result<void>::ok(); }
    virtual Result<std::optional<Chunk>> next() = 0;
    virtual void close() {}

    virtual const Schema& output_schema() const = 0;
};

} // namespace nyx
