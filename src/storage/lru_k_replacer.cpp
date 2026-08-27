#include "storage/lru_k_replacer.h"

namespace nyx {

LRUKReplacer::LRUKReplacer(usize num_frames, usize k) : k_(k), frames_(num_frames) {}

bool LRUKReplacer::victim(FrameId& out) {
    if (evictable_count_ == 0)
        return false;

    FrameId under_k_victim = 0;
    u64 under_k_ts         = 0;
    bool has_under_k       = false;

    FrameId k_victim = 0;
    u64 k_ts         = 0;
    bool has_k       = false;

    for (usize i = 0; i < frames_.size(); ++i) {
        const FrameData& f = frames_[i];
        if (!f.tracked || !f.evictable)
            continue;

        u64 front = f.history.front();
        if (f.history.size() < k_) {
            if (!has_under_k || front < under_k_ts) {
                under_k_victim = static_cast<FrameId>(i);
                under_k_ts     = front;
                has_under_k    = true;
            }
        } else {
            if (!has_k || front < k_ts) {
                k_victim = static_cast<FrameId>(i);
                k_ts     = front;
                has_k    = true;
            }
        }
    }

    out = has_under_k ? under_k_victim : k_victim;
    remove(out);
    return true;
}

void LRUKReplacer::pin(FrameId id) {
    FrameData& f = frames_[id];
    if (!f.tracked)
        return;
    if (f.evictable) {
        f.evictable = false;
        --evictable_count_;
    }
}

void LRUKReplacer::unpin(FrameId id) {
    FrameData& f = frames_[id];
    if (!f.tracked)
        return;
    if (!f.evictable) {
        f.evictable = true;
        ++evictable_count_;
    }
}

void LRUKReplacer::record_access(FrameId id) {
    FrameData& f = frames_[id];
    f.tracked    = true;
    f.history.push_back(++counter_);
    if (f.history.size() > k_)
        f.history.pop_front();
}

void LRUKReplacer::remove(FrameId id) {
    FrameData& f = frames_[id];
    if (!f.tracked)
        return;
    if (f.evictable)
        --evictable_count_;
    f.evictable = false;
    f.tracked   = false;
    f.history.clear();
}

usize LRUKReplacer::size() const {
    return evictable_count_;
}

} // namespace nyx
