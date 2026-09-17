#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/column_file.h"
#include "storage/disk/schema.h"
#include "storage/disk/value.h"

#include <string>
#include <vector>

namespace nyx {

class Table {
  public:
    static Result<Table> create(const std::string& data_root, const std::string& name,
                                Schema schema);
    static Result<Table> open(const std::string& data_root, const std::string& name);

    ~Table() = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) noexcept = default;
    Table& operator=(Table&&) noexcept = default;

    const std::string& name() const { return name_; }
    const std::string& dir() const { return dir_; }
    const Schema& schema() const { return schema_; }
    size_t column_count() const { return columns_.size(); }
    u64 row_count() const;

    ColumnFile& column(size_t idx) { return columns_[idx]; }
    const ColumnFile& column(size_t idx) const { return columns_[idx]; }

    Result<u64> insert(const std::vector<Value>& row);
    Result<u64> insert_many(const std::vector<std::vector<Value>>& rows);

    Result<void> truncate(u64 target_rows);
    Result<void> flush();
    Result<void> fsync();

    bool has_deletions() const { return !deleted_bitmap_.empty(); }
    const std::vector<u8>& deleted_bitmap() const { return deleted_bitmap_; }
    Result<void> mark_deleted(const std::vector<u64>& row_indices);
    Result<void> clear_deletions();

  private:
    Table(std::string dir, std::string name, Schema schema, std::vector<ColumnFile> columns,
          std::vector<u8> deleted_bitmap);

    std::string dir_;
    std::string name_;
    Schema schema_;
    std::vector<ColumnFile> columns_;
    std::vector<u8> deleted_bitmap_;
};

} // namespace nyx
