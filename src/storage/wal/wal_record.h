#pragma once

#include "common/types.h"

#include <cstddef>

namespace nyx {

static constexpr u8 WAL_MAGIC[4] = {'N', 'W', 'A', 'L'};
static constexpr u16 WAL_VERSION = 0x0001;
static constexpr u8 WAL_TYPE_INSERT = 0x01;
static constexpr u8 WAL_TYPE_CREATE = 0x02;
static constexpr u8 WAL_TYPE_DELETE = 0x03;
static constexpr usize WAL_HEADER_SIZE = 6; // magic(4) + version(2)
static constexpr u64 WAL_CHECKPOINT_BYTES = 64ULL * 1024 * 1024; // 64 MB

static inline void wal_put_u16(u8* dst, u16 v) {
    dst[0] = static_cast<u8>(v & 0xFF);
    dst[1] = static_cast<u8>((v >> 8) & 0xFF);
}

static inline void wal_put_u32(u8* dst, u32 v) {
    dst[0] = static_cast<u8>(v & 0xFF);
    dst[1] = static_cast<u8>((v >> 8) & 0xFF);
    dst[2] = static_cast<u8>((v >> 16) & 0xFF);
    dst[3] = static_cast<u8>((v >> 24) & 0xFF);
}

static inline void wal_put_u64(u8* dst, u64 v) {
    for (int i = 0; i < 8; ++i)
        dst[i] = static_cast<u8>((v >> (i * 8)) & 0xFF);
}

static inline u16 wal_read_u16(const u8* src) {
    return static_cast<u16>(src[0]) | (static_cast<u16>(src[1]) << 8);
}

static inline u32 wal_read_u32(const u8* src) {
    return static_cast<u32>(src[0]) | (static_cast<u32>(src[1]) << 8) |
           (static_cast<u32>(src[2]) << 16) | (static_cast<u32>(src[3]) << 24);
}

static inline u64 wal_read_u64(const u8* src) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<u64>(src[i]) << (i * 8);
    return v;
}

} // namespace nyx
