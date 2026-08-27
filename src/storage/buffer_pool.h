#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/replacer.h"

#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace nyx {

class BufferPool {
  public:
    BufferPool(usize fresh_capacity, usize dirty_capacity, DiskManager& disk, usize k = 2);
    ~BufferPool() = default;

    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    Result<Page*> fetch_page(PageId id);
    Result<Page*> new_page(PageId& out_id);
    Result<void> unpin_page(PageId id, bool dirty);
    Result<void> flush_page(PageId id);
    Result<void> flush_all();

    usize fresh_capacity() const { return fresh_slots_.size(); }
    usize dirty_capacity() const { return dirty_slots_.size(); }

  private:
    enum class PoolTag : u8 { FRESH, DIRTY };

    struct FrameLocation {
        PoolTag pool;
        FrameId slot;
    };

    Result<FrameId> acquire_fresh_slot_locked();
    Result<FrameId> migrate_to_dirty_locked(FrameId fresh_slot);
    Result<void> flush_dirty_pool_locked();

    std::vector<std::unique_ptr<Page>> slab_;
    std::vector<Page*> fresh_slots_;
    std::vector<Page*> dirty_slots_;
    std::queue<FrameId> fresh_free_;
    std::queue<FrameId> dirty_free_;
    std::queue<Page*> page_free_;
    std::unordered_map<PageId, FrameLocation> page_table_;
    std::unique_ptr<Replacer> replacer_;
    DiskManager& disk_;
    mutable std::mutex mu_;
};

} // namespace nyx
