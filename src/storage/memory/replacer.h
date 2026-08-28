#pragma once

#include "common/types.h"

namespace nyx {

using FrameId = u32;

class Replacer {
  public:
    virtual ~Replacer() = default;

    virtual bool victim(FrameId& out) = 0;
    virtual void pin(FrameId id) = 0;
    virtual void unpin(FrameId id) = 0;
    virtual void record_access(FrameId id) = 0;
    virtual void remove(FrameId id) = 0;
    virtual usize size() const = 0;
};

} // namespace nyx
