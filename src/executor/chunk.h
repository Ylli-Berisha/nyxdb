#pragma once

#include "common/types.h"
#include "executor/column_vector.h"

#include <cassert>
#include <utility>
#include <vector>

namespace nyx {

class Chunk {
  public:
    Chunk(size_t row_count, std::vector<ColumnVector> columns)
        : row_count_(row_count), columns_(std::move(columns)) {
        for (const auto& c : columns_) {
            (void)c;
            assert(c.size() == row_count_);
        }
    }

    size_t row_count() const { return row_count_; }
    size_t column_count() const { return columns_.size(); }

    ColumnVector& column(size_t i) {
        assert(i < columns_.size());
        return columns_[i];
    }

    const ColumnVector& column(size_t i) const {
        assert(i < columns_.size());
        return columns_[i];
    }

    auto begin() { return columns_.begin(); }
    auto end() { return columns_.end(); }
    auto begin() const { return columns_.begin(); }
    auto end() const { return columns_.end(); }

    void compact_in_place(const ColumnVector& mask) {
        assert(mask.size() == row_count_);
        for (auto& c : columns_)
            c.compact_in_place(mask);
        row_count_ = columns_.empty() ? 0 : columns_.front().size();
    }

  private:
    size_t row_count_;
    std::vector<ColumnVector> columns_;
};

} // namespace nyx
