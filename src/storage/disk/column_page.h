#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/page.h"
#include "storage/disk/type_id.h"

#include <optional>
#include <string>
#include <string_view>

namespace nyx {

#pragma pack(push, 1)
struct ColumnPageHeader {
    TypeId type;
    u8 flags;
    u16 value_count;
    u16 capacity;
    u16 null_bitmap_bytes;
    u8 min_bytes[8];
    u8 max_bytes[8];
    u16 null_count;
    u16 max_len; // VARCHAR max string length; 0 for other types
    u8 reserved[4];
};
#pragma pack(pop)

static constexpr u8 COL_PAGE_FLAG_NULLABLE = 0x01;
static constexpr u8 COL_PAGE_FLAG_HAS_NULLS = 0x02;
static constexpr usize COL_PAGE_HEADER_SIZE = 32;

static_assert(sizeof(ColumnPageHeader) == COL_PAGE_HEADER_SIZE,
              "ColumnPageHeader must be 32 bytes");

inline usize column_page_capacity(TypeId t, bool nullable, u16 max_len = 0) {
    usize elem = type_size(t, max_len);
    if (elem == 0)
        return 0;
    usize budget = PAGE_PAYLOAD_SIZE - COL_PAGE_HEADER_SIZE;
    if (nullable)
        return budget * 8 / (elem * 8 + 1);
    return budget / elem;
}

class ColumnPage {
  public:
    static void init(Page& page, TypeId type, bool nullable, u16 max_len = 0);
    explicit ColumnPage(Page& page) : page_(page) {}

    TypeId type() const;
    u16 value_count() const;
    u16 capacity() const;
    u16 max_len() const;
    bool nullable() const;
    bool is_full() const;

    void truncate_to(u16 new_count);

    Result<void> append_i32(i32 v);
    Result<void> append_i64(i64 v);
    Result<void> append_f64(f64 v);
    Result<void> append_null();
    Result<void> append_str(std::string_view s);
    Result<void> append_bool(bool v);
    Result<void> append_date(i32 days);
    Result<void> append_timestamp(i64 micros);

    Result<i32> get_i32(u16 slot) const;
    Result<i64> get_i64(u16 slot) const;
    Result<f64> get_f64(u16 slot) const;
    std::string get_str(u16 slot) const;
    Result<bool> get_bool(u16 slot) const;
    Result<i32> get_date(u16 slot) const;
    Result<i64> get_timestamp(u16 slot) const;
    bool is_null(u16 slot) const;

    std::optional<i32> min_i32() const;
    std::optional<i32> max_i32() const;
    std::optional<i64> min_i64() const;
    std::optional<i64> max_i64() const;
    std::optional<f64> min_f64() const;
    std::optional<f64> max_f64() const;
    std::optional<i32> min_date() const;
    std::optional<i32> max_date() const;
    std::optional<i64> min_timestamp() const;
    std::optional<i64> max_timestamp() const;

    u16 null_count() const;
    bool has_nulls() const;

    const byte* value_area() const;
    const byte* null_bitmap() const;
    u16 null_bitmap_size() const;

  private:
    Page& page_;

    ColumnPageHeader* header();
    const ColumnPageHeader* header() const;
    byte* null_bitmap();
    byte* value_area();
};

} // namespace nyx
