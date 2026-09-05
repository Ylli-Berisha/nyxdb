#pragma once

#include "common/types.h"
#include "executor/column_vector.h"
#include "executor/selection_vector.h"

#include <cassert>
#include <optional>
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
    size_t logical_size() const { return sel_ ? sel_->size() : row_count_; }
    size_t column_count() const { return columns_.size(); }

    bool has_sel() const { return sel_.has_value(); }
    const SelectionVector& sel() const {
        assert(sel_);
        return *sel_;
    }
    SelectionVector& sel() {
        assert(sel_);
        return *sel_;
    }
    void set_sel(SelectionVector sv) {
        assert(sv.row_count() == row_count_);
        sel_ = std::move(sv);
    }
    void clear_sel() { sel_.reset(); }

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

    void resize(size_t k) {
        if (sel_) {
            assert(k <= sel_->size());
            sel_->truncate(k);
            return;
        }
        assert(k <= row_count_);
        for (auto& c : columns_)
            c.resize(k);
        row_count_ = k;
    }

    void drop_prefix(size_t n) {
        if (sel_) {
            assert(n <= sel_->size());
            sel_->drop_prefix(n);
            return;
        }
        assert(n <= row_count_);
        for (auto& c : columns_)
            c.drop_prefix(n);
        row_count_ -= n;
    }

    void compact_in_place(const ColumnVector& mask) {
        assert(!sel_);
        assert(mask.size() == row_count_);
        for (auto& c : columns_)
            c.compact_in_place(mask);
        row_count_ = columns_.empty() ? 0 : columns_.front().size();
    }

    void compact_from_sel(const ColumnVector& mask) {
        assert(sel_);
        assert(mask.size() == sel_->size());
        std::vector<u32> keep;
        keep.reserve(sel_->size());
        size_t k = 0;
        sel_->for_each([&](u32 phys) {
            if (!mask.is_null(k) && mask.get_i32(k) != 0)
                keep.push_back(phys);
            ++k;
        });
        auto keep_sel = SelectionVector::from_dense(std::move(keep), static_cast<u32>(row_count_));
        std::vector<ColumnVector> new_cols;
        new_cols.reserve(columns_.size());
        for (const auto& c : columns_)
            new_cols.push_back(ColumnVector::gather_via_sel(c, keep_sel));
        columns_ = std::move(new_cols);
        row_count_ = keep_sel.size();
        sel_.reset();
    }

    void materialize() {
        if (!sel_)
            return;
        std::vector<ColumnVector> new_cols;
        new_cols.reserve(columns_.size());
        for (const auto& c : columns_)
            new_cols.push_back(ColumnVector::gather_via_sel(c, *sel_));
        columns_ = std::move(new_cols);
        row_count_ = sel_->size();
        sel_.reset();
    }

  private:
    size_t row_count_;
    std::vector<ColumnVector> columns_;
    std::optional<SelectionVector> sel_;
};

} // namespace nyx
