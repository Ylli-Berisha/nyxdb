#pragma once

#include "common/types.h"
#include "storage/disk/type_id.h"

#include <cassert>
#include <cstring>
#include <vector>

namespace nyx {

class ColumnVector {
  public:
    static ColumnVector make(TypeId type, size_t size, bool nullable) {
        ColumnVector cv(type, 0, nullable);
        cv.resize(size);
        return cv;
    }

    static ColumnVector empty(TypeId type, bool nullable, size_t reserve = 0) {
        ColumnVector cv(type, 0, nullable);
        if (reserve > 0) {
            cv.data_.reserve(reserve * type_size(type));
            if (nullable)
                cv.null_bitmap_.reserve((reserve + 7) / 8);
        }
        return cv;
    }

    TypeId type() const { return type_; }
    size_t size() const { return size_; }
    bool nullable() const { return nullable_; }
    bool has_nulls() const { return has_nulls_; }

    i32 get_i32(size_t i) const {
        assert(type_ == TypeId::INT32);
        assert(i < size_);
        assert(!is_null(i));
        i32 v;
        std::memcpy(&v, data_.data() + i * sizeof(i32), sizeof(i32));
        return v;
    }

    i64 get_i64(size_t i) const {
        assert(type_ == TypeId::INT64);
        assert(i < size_);
        assert(!is_null(i));
        i64 v;
        std::memcpy(&v, data_.data() + i * sizeof(i64), sizeof(i64));
        return v;
    }

    f64 get_f64(size_t i) const {
        assert(type_ == TypeId::DOUBLE);
        assert(i < size_);
        assert(!is_null(i));
        f64 v;
        std::memcpy(&v, data_.data() + i * sizeof(f64), sizeof(f64));
        return v;
    }

    bool is_null(size_t i) const {
        assert(i < size_);
        if (!nullable_)
            return false;
        return (null_bitmap_[i / 8] >> (i % 8)) & 1u;
    }

    void set_i32(size_t i, i32 v) {
        assert(type_ == TypeId::INT32);
        assert(i < size_);
        std::memcpy(data_.data() + i * sizeof(i32), &v, sizeof(i32));
        if (nullable_)
            null_bitmap_[i / 8] &= ~(1u << (i % 8));
    }

    void set_i64(size_t i, i64 v) {
        assert(type_ == TypeId::INT64);
        assert(i < size_);
        std::memcpy(data_.data() + i * sizeof(i64), &v, sizeof(i64));
        if (nullable_)
            null_bitmap_[i / 8] &= ~(1u << (i % 8));
    }

    void set_f64(size_t i, f64 v) {
        assert(type_ == TypeId::DOUBLE);
        assert(i < size_);
        std::memcpy(data_.data() + i * sizeof(f64), &v, sizeof(f64));
        if (nullable_)
            null_bitmap_[i / 8] &= ~(1u << (i % 8));
    }

    void set_null(size_t i) {
        assert(nullable_);
        assert(i < size_);
        null_bitmap_[i / 8] |= (1u << (i % 8));
        has_nulls_ = true;
    }

    void append_i32(i32 v) {
        assert(type_ == TypeId::INT32);
        size_t old = size_;
        resize(old + 1);
        std::memcpy(data_.data() + old * sizeof(i32), &v, sizeof(i32));
    }

    void append_i64(i64 v) {
        assert(type_ == TypeId::INT64);
        size_t old = size_;
        resize(old + 1);
        std::memcpy(data_.data() + old * sizeof(i64), &v, sizeof(i64));
    }

    void append_f64(f64 v) {
        assert(type_ == TypeId::DOUBLE);
        size_t old = size_;
        resize(old + 1);
        std::memcpy(data_.data() + old * sizeof(f64), &v, sizeof(f64));
    }

    void append_null() {
        assert(nullable_);
        size_t old = size_;
        resize(old + 1);
        null_bitmap_[old / 8] |= (1u << (old % 8));
        has_nulls_ = true;
    }

    byte* data() { return data_.data(); }
    const byte* data() const { return data_.data(); }
    u8* null_bitmap_data() { return null_bitmap_.data(); }
    const u8* null_bitmap_data() const { return null_bitmap_.data(); }
    size_t null_bitmap_bytes() const { return null_bitmap_.size(); }

    void resize(size_t new_size) {
        data_.resize(new_size * type_size(type_));
        if (nullable_)
            null_bitmap_.resize((new_size + 7) / 8);
        size_ = new_size;
    }

    void set_has_nulls(bool v) { has_nulls_ = v; }

  private:
    ColumnVector(TypeId type, size_t size, bool nullable)
        : type_(type), nullable_(nullable), has_nulls_(false), size_(size) {}

    TypeId type_;
    bool nullable_;
    bool has_nulls_;
    size_t size_;
    std::vector<byte> data_;
    std::vector<u8> null_bitmap_;
};

} // namespace nyx
