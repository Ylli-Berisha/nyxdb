#pragma once

#include "common/types.h"

#include <cstring>
#include <limits>

namespace nyx {

static constexpr u64 PAGE_SIZE         = 8192;
static constexpr u64 PAGE_HEADER_SIZE  = 24;
static constexpr u64 PAGE_PAYLOAD_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;
static constexpr u64 INVALID_PAGE_ID   = std::numeric_limits<u64>::max();

using PageId = u64;

#pragma pack(push, 1)
struct PageHeader {
    u64 page_id    = INVALID_PAGE_ID;
    u64 checksum   = 0;
    u8 flags       = 0;
    u8 reserved[7] = {};
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == PAGE_HEADER_SIZE, "PageHeader must be 24 bytes");
static_assert(PAGE_SIZE == 8192, "PAGE_SIZE must be 8192 bytes");

struct Page {
    int pin_count        = 0;
    bool dirty           = false;
    byte data[PAGE_SIZE] = {};

    PageHeader* header() { return reinterpret_cast<PageHeader*>(data); }
    const PageHeader* header() const { return reinterpret_cast<const PageHeader*>(data); }

    byte* payload() { return data + PAGE_HEADER_SIZE; }
    const byte* payload() const { return data + PAGE_HEADER_SIZE; }

    PageId page_id() const { return header()->page_id; }

    void reset(PageId id) {
        pin_count = 0;
        dirty     = false;
        std::memset(data, 0, PAGE_SIZE);
        header()->page_id = id;
    }
};

} // namespace nyx
