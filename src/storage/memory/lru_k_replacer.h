#pragma once

#include "storage/memory/replacer.h"

#include <list>
#include <vector>

namespace nyx {

class LRUKReplacer : public Replacer {
  public:
    LRUKReplacer(usize num_frames, usize k);
    ~LRUKReplacer() override = default;

    LRUKReplacer(const LRUKReplacer&) = delete;
    LRUKReplacer& operator=(const LRUKReplacer&) = delete;

    bool victim(FrameId& out) override;
    void pin(FrameId id) override;
    void unpin(FrameId id) override;
    void record_access(FrameId id) override;
    void remove(FrameId id) override;
    usize size() const override;

  private:
    struct FrameData {
        std::list<u64> history;
        bool evictable = false;
        bool tracked = false;
    };

    usize k_;
    u64 counter_ = 0;
    usize evictable_count_ = 0;
    std::vector<FrameData> frames_;
};

} // namespace nyx
