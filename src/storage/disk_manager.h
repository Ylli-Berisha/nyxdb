#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/page.h"

#include <string>

namespace nyx {

class DiskManager {
  public:
    explicit DiskManager(const std::string& path);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    DiskManager(DiskManager&& other) noexcept;
    DiskManager& operator=(DiskManager&& other) noexcept;

    Result<void> read_page(PageId id, Page& out);
    Result<void> write_page(const Page& page);
    Result<PageId> allocate_page();
    Result<void> fsync();

    u64 page_count() const { return next_page_id_; }
    const std::string& path() const { return path_; }

  private:
    int fd_;
    u64 next_page_id_;
    std::string path_;

    off_t offset(PageId id) const { return static_cast<off_t>(id) * PAGE_SIZE; }
};

} // namespace nyx
