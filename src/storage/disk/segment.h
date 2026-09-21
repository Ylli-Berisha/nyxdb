#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/column_file.h"
#include "storage/disk/schema.h"

#include <string>
#include <vector>

namespace nyx {

struct SegmentMeta {
    u64 id;
    u64 base_row_id;
    u64 row_count;
};

class Segment {
  public:
    static Result<Segment> open(const std::string& seg_dir, const Schema& schema,
                                SegmentMeta meta);

    const SegmentMeta& meta() const { return meta_; }
    const std::string& dir() const { return dir_; }
    ColumnFile& column(usize idx) { return columns_[idx]; }
    const ColumnFile& column(usize idx) const { return columns_[idx]; }
    const std::vector<u8>& deleted_bitmap() const { return deleted_; }
    Result<void> mark_deleted(const std::vector<u64>& local_row_ids);

  private:
    Segment(std::string dir, SegmentMeta meta, std::vector<ColumnFile> columns,
            std::vector<u8> deleted);

    std::string dir_;
    SegmentMeta meta_;
    std::vector<ColumnFile> columns_;
    std::vector<u8> deleted_;
};

} // namespace nyx
