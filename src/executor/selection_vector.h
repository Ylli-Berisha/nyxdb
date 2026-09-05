#pragma once

#include "common/types.h"

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace nyx {

class SelectionVector {
  public:
    enum class Repr : u8 {
        DENSE,
        BITMAP,
        RANGES,
    };

    class Builder {
      public:
        void append(u32 idx) {
            if (indices_.empty() || idx != last_ + 1)
                ++run_count_;
            indices_.push_back(idx);
            last_ = idx;
        }

        bool empty() const { return indices_.empty(); }
        size_t size() const { return indices_.size(); }
        u32 run_count() const { return run_count_; }

        SelectionVector finalize(u32 row_count, bool order_matters);

      private:
        std::vector<u32> indices_;
        u32 run_count_ = 0;
        u32 last_ = static_cast<u32>(-1);
    };

    static SelectionVector from_dense(std::vector<u32> indices, u32 row_count);
    static SelectionVector from_bitmap(std::vector<u8> bitmap, u32 row_count, size_t size);
    static SelectionVector from_ranges(std::vector<std::pair<u32, u32>> ranges, u32 row_count);

    Repr repr() const { return repr_; }
    size_t size() const { return size_; }
    u32 row_count() const { return row_count_; }
    bool empty() const { return size_ == 0; }

    u32 at(size_t k) const;

    void truncate(size_t k);

    void drop_prefix(size_t n);

    SelectionVector to_dense() const;

    template <typename F> void for_each(F fn) const {
        switch (repr_) {
        case Repr::DENSE:
            for (u32 i : dense_)
                fn(i);
            break;
        case Repr::BITMAP: {
            for (u32 byte_idx = 0; byte_idx < static_cast<u32>(bitmap_.size()); ++byte_idx) {
                u8 b = bitmap_[byte_idx];
                while (b) {
                    u32 bit = static_cast<u32>(__builtin_ctz(b));
                    fn(byte_idx * 8u + bit);
                    b &= static_cast<u8>(b - 1u);
                }
            }
            break;
        }
        case Repr::RANGES:
            for (auto [start, len] : ranges_)
                for (u32 i = 0; i < len; ++i)
                    fn(start + i);
            break;
        }
    }

    const std::vector<u32>& dense_indices() const {
        assert(repr_ == Repr::DENSE);
        return dense_;
    }
    const std::vector<u8>& bitmap_bytes() const {
        assert(repr_ == Repr::BITMAP);
        return bitmap_;
    }
    const std::vector<std::pair<u32, u32>>& ranges() const {
        assert(repr_ == Repr::RANGES);
        return ranges_;
    }

  private:
    SelectionVector() = default;

    Repr repr_ = Repr::DENSE;
    u32 row_count_ = 0;
    size_t size_ = 0;
    std::vector<u32> dense_;
    std::vector<u8> bitmap_;
    std::vector<std::pair<u32, u32>> ranges_;
};

} // namespace nyx
