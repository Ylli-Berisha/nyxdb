#include "storage/buffer_pool.h"

#include "storage/lru_k_replacer.h"

namespace nyx {

BufferPool::BufferPool(usize fresh_capacity, usize dirty_capacity, DiskManager& disk, usize k)
    : fresh_slots_(fresh_capacity, nullptr), dirty_slots_(dirty_capacity, nullptr), disk_(disk) {

    usize total = fresh_capacity + dirty_capacity;
    slab_.reserve(total);
    for (usize i = 0; i < total; ++i) {
        slab_.emplace_back(std::make_unique<Page>());
        page_free_.push(slab_.back().get());
    }
    for (FrameId i = 0; i < fresh_capacity; ++i)
        fresh_free_.push(i);
    for (FrameId i = 0; i < dirty_capacity; ++i)
        dirty_free_.push(i);

    replacer_ = std::make_unique<LRUKReplacer>(fresh_capacity, k);
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

Result<FrameId> BufferPool::migrate_to_dirty_locked(FrameId fresh_slot) {
    if (dirty_free_.empty()) {
        auto res = flush_dirty_pool_locked();
        if (res.is_err())
            return Result<FrameId>::err(res.error().message);
        if (dirty_free_.empty())
            return Result<FrameId>::err("migrate_to_dirty: dirty pool full and all pages pinned");
    }

    FrameId dirty_slot = dirty_free_.front();
    dirty_free_.pop();
    dirty_slots_[dirty_slot] = fresh_slots_[fresh_slot];
    fresh_slots_[fresh_slot] = nullptr;
    replacer_->remove(fresh_slot);
    fresh_free_.push(fresh_slot);
    return Result<FrameId>::ok(dirty_slot);
}

Result<void> BufferPool::flush_dirty_pool_locked() {
    for (usize i = 0; i < dirty_slots_.size(); ++i) {
        Page* p = dirty_slots_[i];
        if (p == nullptr)
            continue;
        if (p->pin_count > 0)
            continue;

        auto res = disk_.write_page(*p);
        if (res.is_err())
            return res;

        page_table_.erase(p->page_id());
        dirty_slots_[i] = nullptr;
        p->reset(INVALID_PAGE_ID);
        page_free_.push(p);
        dirty_free_.push(static_cast<FrameId>(i));
    }
    return Result<void>::ok();
}

Result<Page*> BufferPool::fetch_page(PageId id) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = page_table_.find(id);
    if (it != page_table_.end()) {
        FrameLocation loc = it->second;
        Page* p = (loc.pool == PoolTag::FRESH) ? fresh_slots_[loc.slot] : dirty_slots_[loc.slot];
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
    p->pin_count       = 1;
    page_table_[id]    = {PoolTag::FRESH, slot};
    replacer_->record_access(slot);
    replacer_->pin(slot);
    return Result<Page*>::ok(p);
}

Result<Page*> BufferPool::new_page(PageId& out_id) {
    std::lock_guard<std::mutex> lock(mu_);

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
    p->pin_count       = 1;
    fresh_slots_[slot] = p;
    page_table_[id]    = {PoolTag::FRESH, slot};
    replacer_->record_access(slot);
    replacer_->pin(slot);

    out_id = id;
    return Result<Page*>::ok(p);
}

Result<void> BufferPool::unpin_page(PageId id, bool dirty) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = page_table_.find(id);
    if (it == page_table_.end())
        return Result<void>::err("unpin_page: page " + std::to_string(id) + " not in pool");

    FrameLocation loc = it->second;
    Page* p = (loc.pool == PoolTag::FRESH) ? fresh_slots_[loc.slot] : dirty_slots_[loc.slot];

    if (p->pin_count <= 0)
        return Result<void>::err("unpin_page: page " + std::to_string(id) + " not pinned");

    p->pin_count--;

    if (dirty && loc.pool == PoolTag::FRESH) {
        auto mig_res = migrate_to_dirty_locked(loc.slot);
        if (mig_res.is_err())
            return Result<void>::err(mig_res.error().message);
        page_table_[id] = {PoolTag::DIRTY, mig_res.value()};
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
    if (loc.pool != PoolTag::DIRTY)
        return Result<void>::ok();

    Page* p  = dirty_slots_[loc.slot];
    auto res = disk_.write_page(*p);
    if (res.is_err())
        return res;

    page_table_.erase(id);
    dirty_slots_[loc.slot] = nullptr;
    p->reset(INVALID_PAGE_ID);
    page_free_.push(p);
    dirty_free_.push(loc.slot);
    return Result<void>::ok();
}

Result<void> BufferPool::flush_all() {
    std::lock_guard<std::mutex> lock(mu_);
    return flush_dirty_pool_locked();
}

} // namespace nyx
