#include "storage/memory/buffer_pool.h"

#include "storage/memory/lru_k_replacer.h"

#include <cmath>

namespace nyx {

BufferPool::BufferPool(usize fresh_capacity, usize dirty_capacity, DiskManager& disk, usize k)
    : fresh_slots_(fresh_capacity, nullptr), disk_(disk) {

    usize half_a = (dirty_capacity + 1) / 2;
    usize half_b = dirty_capacity / 2;
    dirty_halves_[0].assign(half_a, nullptr);
    dirty_halves_[1].assign(half_b, nullptr);

    usize total = fresh_capacity + half_a + half_b;
    slab_.reserve(total);
    for (usize i = 0; i < total; ++i) {
        slab_.emplace_back(std::make_unique<Page>());
        page_free_.push(slab_.back().get());
    }
    for (FrameId i = 0; i < fresh_capacity; ++i)
        fresh_free_.push(i);
    for (FrameId i = 0; i < half_a; ++i)
        dirty_free_[0].push(i);
    for (FrameId i = 0; i < half_b; ++i)
        dirty_free_[1].push(i);

    usize active_size = dirty_halves_[active_half_].size();
    usize high_water_cnt = static_cast<usize>(std::ceil(DIRTY_WATERMARK_PCT * active_size));
    watermark_low_free_ = (active_size > high_water_cnt) ? (active_size - high_water_cnt) : 0;

    replacer_ = std::make_unique<LRUKReplacer>(fresh_capacity, k);

    bg_thread_ = std::thread(&BufferPool::bg_worker, this);
}

BufferPool::~BufferPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        bg_stop_ = true;
    }
    cv_.notify_all();
    if (bg_thread_.joinable())
        bg_thread_.join();
}

void BufferPool::bg_worker() {
    while (true) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return bg_stop_ || flush_state_ == FlushState::FLUSHING; });
        if (bg_stop_)
            return;

        u8 immutable = 1 - active_half_;
        (void)flush_half_locked(immutable);

        flush_state_ = FlushState::IDLE;
        lock.unlock();
        cv_.notify_all();
    }
}

Result<FrameId> BufferPool::acquire_fresh_slot_locked() {
    if (!fresh_free_.empty()) {
        FrameId slot = fresh_free_.front();
        fresh_free_.pop();
        return Result<FrameId>::ok(slot);
    }

    FrameId victim;
    if (!replacer_->victim(victim))
        return Result<FrameId>::err("acquire_fresh_slot: fresh pool full, no evictable frame");

    Page* p = fresh_slots_[victim];
    page_table_.erase(p->page_id());
    fresh_slots_[victim] = nullptr;
    p->reset(INVALID_PAGE_ID);
    page_free_.push(p);
    return Result<FrameId>::ok(victim);
}

void BufferPool::rotate_dirty_pool_locked() {
    active_half_ = 1 - active_half_;
    flush_state_ = FlushState::FLUSHING;
    cv_.notify_one();
}

Result<void> BufferPool::flush_half_locked(u8 half) {
    auto& slots = dirty_halves_[half];
    for (usize i = 0; i < slots.size(); ++i) {
        Page* p = slots[i];
        if (p == nullptr)
            continue;
        if (p->pin_count > 0)
            continue;

        auto res = disk_.write_page(*p);
        if (res.is_err())
            return res;

        page_table_.erase(p->page_id());
        slots[i] = nullptr;
        p->reset(INVALID_PAGE_ID);
        page_free_.push(p);
        dirty_free_[half].push(static_cast<FrameId>(i));
    }
    return Result<void>::ok();
}

Result<void> BufferPool::flush_dirty_pool_locked() {
    auto res = flush_half_locked(0);
    if (res.is_err())
        return res;
    return flush_half_locked(1);
}

Result<FrameId> BufferPool::migrate_to_dirty_locked(std::unique_lock<std::mutex>& lock,
                                                    FrameId fresh_slot, PoolTag& out_tag) {
    while (dirty_free_[active_half_].empty() && flush_state_ == FlushState::FLUSHING)
        cv_.wait(lock);

    if (flush_state_ == FlushState::IDLE && dirty_free_[active_half_].size() <= watermark_low_free_)
        rotate_dirty_pool_locked();

    if (dirty_free_[active_half_].empty())
        return Result<FrameId>::err("migrate_to_dirty: both dirty halves full of pinned pages");

    u8 half = active_half_;
    FrameId d_slot = dirty_free_[half].front();
    dirty_free_[half].pop();

    dirty_halves_[half][d_slot] = fresh_slots_[fresh_slot];
    fresh_slots_[fresh_slot] = nullptr;
    replacer_->remove(fresh_slot);
    fresh_free_.push(fresh_slot);

    out_tag = dirty_tag(half);
    return Result<FrameId>::ok(d_slot);
}

Result<Page*> BufferPool::fetch_page(PageId id) {
    std::unique_lock<std::mutex> lock(mu_);

    auto it = page_table_.find(id);
    if (it != page_table_.end()) {
        FrameLocation loc = it->second;
        Page* p = (loc.pool == PoolTag::FRESH) ? fresh_slots_[loc.slot]
                                               : dirty_halves_[half_index(loc.pool)][loc.slot];
        p->pin_count++;
        if (loc.pool == PoolTag::FRESH) {
            replacer_->record_access(loc.slot);
            replacer_->pin(loc.slot);
        }
        return Result<Page*>::ok(p);
    }

    auto slot_res = acquire_fresh_slot_locked();
    if (slot_res.is_err())
        return Result<Page*>::err(slot_res.error().message);
    FrameId slot = slot_res.value();

    Page* p = page_free_.front();
    page_free_.pop();

    auto read_res = disk_.read_page(id, *p);
    if (read_res.is_err()) {
        page_free_.push(p);
        fresh_free_.push(slot);
        return Result<Page*>::err(read_res.error().message);
    }

    fresh_slots_[slot] = p;
    p->pin_count = 1;
    page_table_[id] = {PoolTag::FRESH, slot};
    replacer_->record_access(slot);
    replacer_->pin(slot);
    return Result<Page*>::ok(p);
}

Result<Page*> BufferPool::new_page(PageId& out_id) {
    std::unique_lock<std::mutex> lock(mu_);

    auto slot_res = acquire_fresh_slot_locked();
    if (slot_res.is_err())
        return Result<Page*>::err(slot_res.error().message);
    FrameId slot = slot_res.value();

    auto id_res = disk_.allocate_page();
    if (id_res.is_err()) {
        fresh_free_.push(slot);
        return Result<Page*>::err(id_res.error().message);
    }
    PageId id = id_res.value();

    Page* p = page_free_.front();
    page_free_.pop();
    p->reset(id);
    p->pin_count = 1;
    fresh_slots_[slot] = p;
    page_table_[id] = {PoolTag::FRESH, slot};
    replacer_->record_access(slot);
    replacer_->pin(slot);

    out_id = id;
    return Result<Page*>::ok(p);
}

Result<void> BufferPool::unpin_page(PageId id, bool dirty) {
    std::unique_lock<std::mutex> lock(mu_);

    auto it = page_table_.find(id);
    if (it == page_table_.end())
        return Result<void>::err("unpin_page: page " + std::to_string(id) + " not in pool");

    FrameLocation loc = it->second;
    Page* p = (loc.pool == PoolTag::FRESH) ? fresh_slots_[loc.slot]
                                           : dirty_halves_[half_index(loc.pool)][loc.slot];

    if (p->pin_count <= 0)
        return Result<void>::err("unpin_page: page " + std::to_string(id) + " not pinned");

    p->pin_count--;

    if (dirty && loc.pool == PoolTag::FRESH) {
        PoolTag new_tag;
        auto mig_res = migrate_to_dirty_locked(lock, loc.slot, new_tag);
        if (mig_res.is_err())
            return Result<void>::err(mig_res.error().message);
        page_table_[id] = {new_tag, mig_res.value()};
        return Result<void>::ok();
    }

    if (p->pin_count == 0 && loc.pool == PoolTag::FRESH)
        replacer_->unpin(loc.slot);

    return Result<void>::ok();
}

Result<void> BufferPool::flush_page(PageId id) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = page_table_.find(id);
    if (it == page_table_.end())
        return Result<void>::err("flush_page: page " + std::to_string(id) + " not in pool");

    FrameLocation loc = it->second;
    if (loc.pool == PoolTag::FRESH)
        return Result<void>::ok();

    u8 half = half_index(loc.pool);
    Page* p = dirty_halves_[half][loc.slot];
    auto res = disk_.write_page(*p);
    if (res.is_err())
        return res;

    page_table_.erase(id);
    dirty_halves_[half][loc.slot] = nullptr;
    p->reset(INVALID_PAGE_ID);
    page_free_.push(p);
    dirty_free_[half].push(loc.slot);
    return Result<void>::ok();
}

Result<void> BufferPool::flush_all() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return flush_state_ == FlushState::IDLE; });
    return flush_half_locked(active_half_);
}

} // namespace nyx
