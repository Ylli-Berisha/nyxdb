#include "executor/selection_vector.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace nyx {

namespace {

constexpr double BITMAP_DENSITY_THRESHOLD = 0.9;
constexpr double RANGES_AVG_RUN_THRESHOLD = 32.0;

std::vector<u8> indices_to_bitmap(const std::vector<u32>& indices, u32 row_count) {
    size_t bytes = (row_count + 7u) / 8u;
    std::vector<u8> bm(bytes, 0);
    for (u32 idx : indices)
        bm[idx / 8u] |= static_cast<u8>(1u << (idx % 8u));
    return bm;
}

std::vector<std::pair<u32, u32>> indices_to_ranges(const std::vector<u32>& indices) {
    std::vector<std::pair<u32, u32>> out;
    if (indices.empty())
        return out;
    u32 start = indices.front();
    u32 len = 1;
    for (size_t i = 1; i < indices.size(); ++i) {
        if (indices[i] == indices[i - 1] + 1) {
            ++len;
        } else {
            out.emplace_back(start, len);
            start = indices[i];
            len = 1;
        }
    }
    out.emplace_back(start, len);
    return out;
}

} // namespace

SelectionVector SelectionVector::Builder::finalize(u32 row_count, bool order_matters) {
    SelectionVector sv;
    sv.row_count_ = row_count;
    sv.size_ = indices_.size();

    if (order_matters || sv.size_ == 0) {
        sv.repr_ = Repr::DENSE;
        sv.dense_ = std::move(indices_);
        return sv;
    }

    double density = static_cast<double>(sv.size_) / static_cast<double>(row_count);
    if (density > BITMAP_DENSITY_THRESHOLD) {
        sv.repr_ = Repr::BITMAP;
        sv.bitmap_ = indices_to_bitmap(indices_, row_count);
        return sv;
    }

    double avg_run = static_cast<double>(sv.size_) / static_cast<double>(std::max(1u, run_count_));
    if (avg_run > RANGES_AVG_RUN_THRESHOLD) {
        auto ranges = indices_to_ranges(indices_);
        size_t ranges_bytes = ranges.size() * sizeof(std::pair<u32, u32>);
        size_t dense_bytes = indices_.size() * sizeof(u32);
        if (ranges_bytes < dense_bytes) {
            sv.repr_ = Repr::RANGES;
            sv.ranges_ = std::move(ranges);
            return sv;
        }
    }

    sv.repr_ = Repr::DENSE;
    sv.dense_ = std::move(indices_);
    return sv;
}

SelectionVector SelectionVector::from_dense(std::vector<u32> indices, u32 row_count) {
    SelectionVector sv;
    sv.repr_ = Repr::DENSE;
    sv.row_count_ = row_count;
    sv.size_ = indices.size();
    sv.dense_ = std::move(indices);
    return sv;
}

SelectionVector SelectionVector::from_bitmap(std::vector<u8> bitmap, u32 row_count, size_t size) {
    SelectionVector sv;
    sv.repr_ = Repr::BITMAP;
    sv.row_count_ = row_count;
    sv.size_ = size;
    sv.bitmap_ = std::move(bitmap);
    return sv;
}

SelectionVector SelectionVector::from_ranges(std::vector<std::pair<u32, u32>> ranges,
                                             u32 row_count) {
    SelectionVector sv;
    sv.repr_ = Repr::RANGES;
    sv.row_count_ = row_count;
    size_t total = 0;
    for (auto& [_, len] : ranges) {
        (void)_;
        total += len;
    }
    sv.size_ = total;
    sv.ranges_ = std::move(ranges);
    return sv;
}

u32 SelectionVector::at(size_t k) const {
    assert(k < size_);
    switch (repr_) {
    case Repr::DENSE:
        return dense_[k];
    case Repr::RANGES: {
        size_t cumulative = 0;
        for (auto [start, len] : ranges_) {
            if (k < cumulative + len)
                return start + static_cast<u32>(k - cumulative);
            cumulative += len;
        }
        assert(false && "unreachable");
        return 0;
    }
    case Repr::BITMAP:
        assert(false && "at() unsupported for BITMAP — use for_each or to_dense first");
        return 0;
    }
    return 0;
}

void SelectionVector::truncate(size_t k) {
    if (k >= size_)
        return;
    switch (repr_) {
    case Repr::DENSE:
        dense_.resize(k);
        break;
    case Repr::BITMAP: {
        size_t kept = 0;
        for (u32 byte_idx = 0; byte_idx < bitmap_.size(); ++byte_idx) {
            u8 b = bitmap_[byte_idx];
            u32 pop = static_cast<u32>(__builtin_popcount(b));
            if (kept + pop <= k) {
                kept += pop;
                continue;
            }
            u32 need = static_cast<u32>(k - kept);
            u8 kept_bits = 0;
            u32 taken = 0;
            u8 cur = b;
            while (cur && taken < need) {
                u32 bit = static_cast<u32>(__builtin_ctz(cur));
                kept_bits |= static_cast<u8>(1u << bit);
                cur &= static_cast<u8>(cur - 1u);
                ++taken;
            }
            bitmap_[byte_idx] = kept_bits;
            for (u32 later = byte_idx + 1; later < bitmap_.size(); ++later)
                bitmap_[later] = 0;
            break;
        }
        break;
    }
    case Repr::RANGES: {
        size_t cumulative = 0;
        for (size_t i = 0; i < ranges_.size(); ++i) {
            auto& r = ranges_[i];
            if (cumulative + r.second <= k) {
                cumulative += r.second;
                continue;
            }
            r.second = static_cast<u32>(k - cumulative);
            ranges_.resize(i + 1);
            break;
        }
        break;
    }
    }
    size_ = k;
}

void SelectionVector::drop_prefix(size_t n) {
    if (n == 0)
        return;
    assert(n <= size_);
    switch (repr_) {
    case Repr::DENSE:
        dense_.erase(dense_.begin(), dense_.begin() + n);
        break;
    case Repr::BITMAP: {
        std::vector<u32> new_dense;
        new_dense.reserve(size_ - n);
        size_t seen = 0;
        for (u32 byte_idx = 0; byte_idx < bitmap_.size(); ++byte_idx) {
            u8 b = bitmap_[byte_idx];
            while (b) {
                u32 bit = static_cast<u32>(__builtin_ctz(b));
                if (seen >= n)
                    new_dense.push_back(byte_idx * 8u + bit);
                ++seen;
                b &= static_cast<u8>(b - 1u);
            }
        }
        bitmap_.clear();
        repr_ = Repr::DENSE;
        dense_ = std::move(new_dense);
        break;
    }
    case Repr::RANGES: {
        size_t to_skip = n;
        size_t drop_up_to = 0;
        for (size_t i = 0; i < ranges_.size(); ++i) {
            auto& r = ranges_[i];
            if (r.second <= to_skip) {
                to_skip -= r.second;
                drop_up_to = i + 1;
                continue;
            }
            r.first += static_cast<u32>(to_skip);
            r.second -= static_cast<u32>(to_skip);
            break;
        }
        if (drop_up_to > 0)
            ranges_.erase(ranges_.begin(), ranges_.begin() + drop_up_to);
        break;
    }
    }
    size_ -= n;
}

SelectionVector SelectionVector::to_dense() const {
    if (repr_ == Repr::DENSE)
        return *this;
    std::vector<u32> out;
    out.reserve(size_);
    for_each([&](u32 i) { out.push_back(i); });
    return from_dense(std::move(out), row_count_);
}

} // namespace nyx
