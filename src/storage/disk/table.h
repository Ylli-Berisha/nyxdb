#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/column_file.h"
#include "storage/disk/schema.h"
#include "storage/disk/segment.h"
#include "storage/disk/value.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace nyx {

class Table {
  public:
    static Result<Table> create(const std::string& data_root, const std::string& name,
                                Schema schema, u64 flush_threshold = FLUSH_THRESHOLD);
    static Result<Table> open(const std::string& data_root, const std::string& name);

    ~Table() = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) noexcept = default;
    Table& operator=(Table&&) noexcept = default;

    const std::string& name() const { return name_; }
    const std::string& dir() const { return dir_; }
    const Schema& schema() const { return schema_; }
    size_t column_count() const { return wb_columns_.size(); }
    u64 row_count() const;

    ColumnFile& column(size_t idx) { return wb_columns_[idx]; }
    const ColumnFile& column(size_t idx) const { return wb_columns_[idx]; }

    Result<u64> insert(const std::vector<Value>& row);
    Result<u64> insert_many(const std::vector<std::vector<Value>>& rows);

    Result<void> truncate(u64 target_rows);
    Result<void> flush();
    Result<void> fsync();

    bool has_deletions() const { return !wb_deleted_.empty(); }
    const std::vector<u8>& deleted_bitmap() const { return wb_deleted_; }
    bool is_row_deleted(u64 global_id) const;
    Result<void> mark_deleted(const std::vector<u64>& row_indices);
    Result<void> clear_deletions();

    struct UpdateMeta {
        u64 first_row_id;
        u64 wb_base_before;
        u64 wb_base_after;
    };
    Result<UpdateMeta> update_rows(const std::vector<u64>& old_indices,
                                   const std::vector<std::vector<Value>>& new_rows);

    const std::vector<Segment>& segments() const { return segments_; }
    u64 wb_base_row_id() const { return wb_base_row_id_; }
    const std::vector<ColumnFile>& wb_columns_ref() const { return wb_columns_; }
    const std::vector<u8>& wb_deleted_ref() const { return wb_deleted_; }

    [[nodiscard]] std::shared_lock<std::shared_mutex> lock_shared() const {
        return std::shared_lock<std::shared_mutex>(*rwlock_);
    }

    Result<void> replace_segments(const std::vector<usize>& indices, const std::string& tmp_dir,
                                  u64 merged_base_row_id, u64 merged_row_count);

  private:
    Table(std::string dir, std::string name, Schema schema, std::vector<ColumnFile> wb_columns,
          std::vector<u8> wb_deleted, u64 wb_base_row_id, std::vector<Segment> segments,
          u64 next_segment_id, u64 flush_threshold = FLUSH_THRESHOLD);

    Result<void> maybe_flush_();
    Result<void> flush_write_buffer_();
    Result<void> mark_deleted_nolock_(const std::vector<u64>& row_indices);
    Result<u64> insert_many_nolock_(const std::vector<std::vector<Value>>& rows);

    std::string dir_;
    std::string name_;
    Schema schema_;
    std::vector<ColumnFile> wb_columns_;
    std::vector<u8> wb_deleted_;
    u64 wb_base_row_id_ = 0;
    std::vector<Segment> segments_;
    u64 next_segment_id_ = 0;
    u64 flush_threshold_;
    mutable std::unique_ptr<std::shared_mutex> rwlock_;

    static constexpr u64 FLUSH_THRESHOLD = 65536;
};

} // namespace nyx
