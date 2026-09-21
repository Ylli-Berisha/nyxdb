#include "storage/disk/segment.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nyx {

static std::vector<u8> seg_load_deleted(const std::string& dir) {
    std::string path = dir + "/deleted.bin";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return {};
    struct stat st {};
    ::fstat(fd, &st);
    std::vector<u8> bm(static_cast<usize>(st.st_size));
    if (!bm.empty()) {
        ssize_t n = ::read(fd, bm.data(), bm.size());
        if (n != static_cast<ssize_t>(bm.size()))
            bm.clear();
    }
    ::close(fd);
    return bm;
}

static Result<void> seg_write_deleted(const std::string& dir, const std::vector<u8>& bm) {
    std::string path = dir + "/deleted.bin";
    if (bm.empty()) {
        ::unlink(path.c_str());
        return Result<void>::ok();
    }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Result<void>::err("segment: open deleted.bin: " + std::string(strerror(errno)));
    ssize_t n = ::write(fd, bm.data(), bm.size());
    ::fsync(fd);
    ::close(fd);
    if (n != static_cast<ssize_t>(bm.size()))
        return Result<void>::err("segment: short write deleted.bin");
    return Result<void>::ok();
}

Segment::Segment(std::string dir, SegmentMeta meta, std::vector<ColumnFile> columns,
                 std::vector<u8> deleted)
    : dir_(std::move(dir)), meta_(meta), columns_(std::move(columns)),
      deleted_(std::move(deleted)) {}

Result<Segment> Segment::open(const std::string& seg_dir, const Schema& schema, SegmentMeta meta) {
    std::vector<ColumnFile> columns;
    columns.reserve(schema.size());
    for (const auto& col : schema) {
        std::string col_path = seg_dir + "/" + col.name + ".col";
        auto cf_r = ColumnFile::open(col_path);
        if (cf_r.is_err())
            return Result<Segment>::err("segment open col '" + col.name +
                                        "': " + cf_r.error().message);
        columns.push_back(std::move(cf_r.value()));
    }
    auto deleted = seg_load_deleted(seg_dir);
    return Result<Segment>::ok(Segment(seg_dir, meta, std::move(columns), std::move(deleted)));
}

Result<void> Segment::mark_deleted(const std::vector<u64>& local_row_ids) {
    if (local_row_ids.empty())
        return Result<void>::ok();
    usize bytes_needed = static_cast<usize>((meta_.row_count + 7) / 8);
    if (deleted_.size() < bytes_needed)
        deleted_.resize(bytes_needed, 0);
    for (u64 id : local_row_ids) {
        if (id < meta_.row_count)
            deleted_[id / 8] |= static_cast<u8>(1u << (id % 8));
    }
    return seg_write_deleted(dir_, deleted_);
}

} // namespace nyx
