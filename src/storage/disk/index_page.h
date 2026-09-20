#pragma once

#include "common/types.h"
#include "storage/disk/page.h"
#include "storage/disk/type_id.h"

namespace nyx {

enum IndexPageType : u8 {
    IDX_PAGE_HEADER = 0,
    IDX_PAGE_INTERNAL = 1,
    IDX_PAGE_LEAF = 2,
};

static constexpr u16 INDEX_PAGE_HDR_SIZE = 8;
static constexpr u32 INDEX_FILE_MAGIC = 0x4958594Eu;

static constexpr u8 IDX_FLAG_IS_ROOT = 0x01u;

#pragma pack(push, 1)

struct IndexPageHeader {
    u8 page_type;
    u8 flags;
    u16 entry_count;
    u16 capacity;
    u8 reserved[2];
};
static_assert(sizeof(IndexPageHeader) == INDEX_PAGE_HDR_SIZE);

struct IndexColSpec {
    TypeId type;
    u8 reserved;
    u16 max_len;
};
static_assert(sizeof(IndexColSpec) == 4);

struct IndexFileMeta {
    u32 magic;
    u8 dirty;
    u8 is_unique;
    u8 col_count;
    u8 reserved[1];
    u64 root_page_id;
    u64 entry_count;
};
static_assert(sizeof(IndexFileMeta) == 24);

#pragma pack(pop)

inline usize index_col_bytes(TypeId t, u16 max_len) {
    return 1u + type_size(t, max_len);
}

inline u16 index_node_capacity(usize key_size) {
    usize avail = static_cast<usize>(PAGE_PAYLOAD_SIZE) - INDEX_PAGE_HDR_SIZE - 8u;
    usize n = (key_size + 8u > 0u) ? avail / (key_size + 8u) : 0u;
    return n > 65535u ? u16{65535u} : static_cast<u16>(n);
}

} // namespace nyx
