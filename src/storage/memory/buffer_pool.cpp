#include "storage/memory/buffer_pool.h"

#include "storage/memory/lru_k_replacer.h"

namespace nyx {

BufferPool::BufferPool(usize fresh_capacity, DiskManager& disk, usize k)
    : fresh_slots_(fresh_capacity, nullptr), disk_(disk) {

    slab_.reserve(fresh_capacity);
    for (usize i = 0; i < fresh_capacity; ++i) {
        slab_.emplace_back(std::make_unique<Page>());
        page_free_.push(slab_.back().get());
    }
    for (FrameId i = 0; i < fresh_capacity; ++i)
        fresh_free_.push(i);

    replacer_ = std::make_unique<LRUKReplacer>(fresh_capacity, k);
}

BufferPool::~BufferPool() = default;

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

Result<Page*> BufferPool::fetch_page(PageId id) {
    std::unique_lock<std::mutex> lock(mu_);

    auto it = page_table_.find(id);
    if (it != page_table_.end()) {
        FrameId slot = it->second;
        Page* p = fresh_slots_[slot];
        p->pin_count++;
        replacer_->record_access(slot);
        replacer_->pin(slot);
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
    page_table_[id] = slot;
    replacer_->record_access(slot);
    replacer_->pin(slot);
    return Result<Page*>::ok(p);
}

Result<void> BufferPool::unpin_page(PageId id) {
    std::unique_lock<std::mutex> lock(mu_);

    auto it = page_table_.find(id);
    if (it == page_table_.end())
        return Result<void>::err("unpin_page: page " + std::to_string(id) + " not in pool");

    FrameId slot = it->second;
    Page* p = fresh_slots_[slot];

    if (p->pin_count <= 0)
        return Result<void>::err("unpin_page: page " + std::to_string(id) + " not pinned");

    p->pin_count--;
    if (p->pin_count == 0)
        replacer_->unpin(slot);

    return Result<void>::ok();
}

} // namespace nyx
