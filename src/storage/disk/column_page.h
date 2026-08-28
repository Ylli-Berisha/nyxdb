#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/page.h"
#include "storage/disk/type_id.h"

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
    u8 reserved[8];
};
#pragma pack(pop)

static constexpr u8 COL_PAGE_FLAG_NULLABLE = 0x01;
static constexpr usize COL_PAGE_HEADER_SIZE = 32;

static_assert(sizeof(ColumnPageHeader) == COL_PAGE_HEADER_SIZE,
              "ColumnPageHeader must be 32 bytes");

constexpr usize column_page_capacity(TypeId t, bool nullable) {
    usize elem = type_size(t);
    usize budget = PAGE_PAYLOAD_SIZE - COL_PAGE_HEADER_SIZE;
    if (nullable)
        return budget * 8 / (elem * 8 + 1);
    return budget / elem;
}

class ColumnPage {
  public:
    static void init(Page& page, TypeId type, bool nullable);
    explicit ColumnPage(Page& page) : page_(page) {}

    TypeId type() const;
    u16 value_count() const;
    u16 capacity() const;
    bool nullable() const;
    bool is_full() const;

    Result<void> append_i32(i32 v);
    Result<void> append_i64(i64 v);
    Result<void> append_f64(f64 v);
    Result<void> append_null();

    Result<i32> get_i32(u16 slot) const;
    Result<i64> get_i64(u16 slot) const;
    Result<f64> get_f64(u16 slot) const;
    bool is_null(u16 slot) const;

  private:
    Page& page_;

    ColumnPageHeader* header();
    const ColumnPageHeader* header() const;
    byte* null_bitmap();
    const byte* null_bitmap() const;
    byte* value_area();
    const byte* value_area() const;
};

} // namespace nyx
