#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/disk_manager.h"
#include "storage/disk/page.h"
#include "storage/memory/replacer.h"

#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace nyx {

class BufferPool {
  public:
    BufferPool(usize fresh_capacity, DiskManager& disk, usize k = 2);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    Result<Page*> fetch_page(PageId id);
    Result<void> unpin_page(PageId id);

    usize fresh_capacity() const { return fresh_slots_.size(); }

  private:
    Result<FrameId> acquire_fresh_slot_locked();

    std::vector<std::unique_ptr<Page>> slab_;
    std::vector<Page*> fresh_slots_;
    std::queue<FrameId> fresh_free_;
    std::queue<Page*> page_free_;
    std::unordered_map<PageId, FrameId> page_table_;
    std::unique_ptr<Replacer> replacer_;
    DiskManager& disk_;
    mutable std::mutex mu_;
};

} // namespace nyx
