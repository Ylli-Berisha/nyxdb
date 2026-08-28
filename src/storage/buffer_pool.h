#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/replacer.h"

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nyx {

class BufferPool {
  public:
    BufferPool(usize fresh_capacity, usize dirty_capacity, DiskManager& disk, usize k = 2);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    Result<Page*> fetch_page(PageId id);
    Result<Page*> new_page(PageId& out_id);
    Result<void> unpin_page(PageId id, bool dirty);
    Result<void> flush_page(PageId id);
    Result<void> flush_all();

    usize fresh_capacity() const { return fresh_slots_.size(); }
    usize dirty_capacity() const { return dirty_halves_[0].size() + dirty_halves_[1].size(); }

  private:
    enum class PoolTag : u8 { FRESH, DIRTY_A, DIRTY_B };
    enum class FlushState : u8 { IDLE, FLUSHING };

    struct FrameLocation {
        PoolTag pool;
        FrameId slot;
    };

    static constexpr double DIRTY_WATERMARK_PCT = 0.75;

    static u8 half_index(PoolTag t) { return (t == PoolTag::DIRTY_A) ? 0 : 1; }
    static PoolTag dirty_tag(u8 half) { return (half == 0) ? PoolTag::DIRTY_A : PoolTag::DIRTY_B; }

    Result<FrameId> acquire_fresh_slot_locked();
    Result<FrameId> migrate_to_dirty_locked(std::unique_lock<std::mutex>& lock, FrameId fresh_slot,
                                            PoolTag& out_tag);
    void rotate_dirty_pool_locked();
    Result<void> flush_half_locked(u8 half);
    Result<void> flush_dirty_pool_locked();
    void bg_worker();

    std::vector<std::unique_ptr<Page>> slab_;
    std::vector<Page*> fresh_slots_;
    std::array<std::vector<Page*>, 2> dirty_halves_;
    std::queue<FrameId> fresh_free_;
    std::array<std::queue<FrameId>, 2> dirty_free_;
    std::queue<Page*> page_free_;
    std::unordered_map<PageId, FrameLocation> page_table_;
    std::unique_ptr<Replacer> replacer_;
    DiskManager& disk_;
    u8 active_half_ = 0;
    usize watermark_low_free_ = 0;
    FlushState flush_state_ = FlushState::IDLE;
    bool bg_stop_ = false;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread bg_thread_;
};

} // namespace nyx
