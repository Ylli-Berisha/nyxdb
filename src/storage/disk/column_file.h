#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/column_page.h"
#include "storage/disk/disk_manager.h"
#include "storage/disk/page.h"
#include "storage/disk/type_id.h"
#include "storage/disk/value.h"

#include <functional>
#include <string>
#include <vector>

namespace nyx {

class ColumnFile {
  public:
    static Result<ColumnFile> create(const std::string& path, TypeId type, bool nullable);
    static Result<ColumnFile> open(const std::string& path);

    ~ColumnFile() = default;
    ColumnFile(const ColumnFile&) = delete;
    ColumnFile& operator=(const ColumnFile&) = delete;
    ColumnFile(ColumnFile&&) noexcept = default;
    ColumnFile& operator=(ColumnFile&&) noexcept = default;

    TypeId type() const { return type_; }
    bool nullable() const { return nullable_; }
    u16 page_capacity() const { return capacity_; }
    u64 row_count() const;
    const std::string& path() const { return disk_.path(); }

    Result<void> append_i32(i32 v);
    Result<void> append_i64(i64 v);
    Result<void> append_f64(f64 v);
    Result<void> append_null();

    Result<void> append_bulk(const std::vector<Value>& values);

    Result<i32> get_i32(u64 row_id);
    Result<i64> get_i64(u64 row_id);
    Result<f64> get_f64(u64 row_id);
    bool is_null(u64 row_id);

    Result<void> scan(std::function<void(const ColumnPage&)> fn);

    Result<void> flush();
    Result<void> fsync();

  private:
    ColumnFile(DiskManager disk, TypeId type, bool nullable, u16 capacity, Page current,
               PageId current_id, bool current_dirty);

    Result<void> ensure_room_for_append();
    Result<void> rotate_page();

    DiskManager disk_;
    TypeId type_;
    bool nullable_;
    u16 capacity_;
    Page current_page_;
    PageId current_page_id_;
    bool current_dirty_;
};

} // namespace nyx
