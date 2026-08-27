#pragma once

#include "common/types.h"

#include <cstring>

namespace nyx {

namespace detail {

static constexpr u64 XXH_PRIME1 = 0x9E3779B185EBCA87ULL;
static constexpr u64 XXH_PRIME2 = 0xC2B2AE3D27D4EB4FULL;
static constexpr u64 XXH_PRIME3 = 0x165667B19E3779F9ULL;
static constexpr u64 XXH_PRIME4 = 0x85EBCA77C2B2AE63ULL;
static constexpr u64 XXH_PRIME5 = 0x27D4EB2F165667C5ULL;

inline u64 rotl64(u64 x, int r) {
    return (x << r) | (x >> (64 - r));
}

inline u64 read64(const byte* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}
inline u32 read32(const byte* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

inline u64 xxh_round(u64 acc, u64 input) {
    acc += input * XXH_PRIME2;
    acc = rotl64(acc, 31);
    acc *= XXH_PRIME1;
    return acc;
}

inline u64 xxh_merge_round(u64 acc, u64 val) {
    val = xxh_round(0, val);
    acc ^= val;
    acc = acc * XXH_PRIME1 + XXH_PRIME4;
    return acc;
}

} // namespace detail

inline u64 xxhash64(const byte* data, usize len, u64 seed = 0) {
    const byte* p = data;
    const byte* end = data + len;
    u64 h64;

    if (len >= 32) {
        const byte* limit = end - 32;
        u64 v1 = seed + detail::XXH_PRIME1 + detail::XXH_PRIME2;
        u64 v2 = seed + detail::XXH_PRIME2;
        u64 v3 = seed;
        u64 v4 = seed - detail::XXH_PRIME1;

        do {
            v1 = detail::xxh_round(v1, detail::read64(p));
            p += 8;
            v2 = detail::xxh_round(v2, detail::read64(p));
            p += 8;
            v3 = detail::xxh_round(v3, detail::read64(p));
            p += 8;
            v4 = detail::xxh_round(v4, detail::read64(p));
            p += 8;
        } while (p <= limit);

        h64 = detail::rotl64(v1, 1) + detail::rotl64(v2, 7) + detail::rotl64(v3, 12) +
              detail::rotl64(v4, 18);

        h64 = detail::xxh_merge_round(h64, v1);
        h64 = detail::xxh_merge_round(h64, v2);
        h64 = detail::xxh_merge_round(h64, v3);
        h64 = detail::xxh_merge_round(h64, v4);
    } else {
        h64 = seed + detail::XXH_PRIME5;
    }

    h64 += static_cast<u64>(len);

    while (p + 8 <= end) {
        u64 k1 = detail::xxh_round(0, detail::read64(p));
        h64 ^= k1;
        h64 = detail::rotl64(h64, 27) * detail::XXH_PRIME1 + detail::XXH_PRIME4;
        p += 8;
    }

    if (p + 4 <= end) {
        h64 ^= static_cast<u64>(detail::read32(p)) * detail::XXH_PRIME1;
        h64 = detail::rotl64(h64, 23) * detail::XXH_PRIME2 + detail::XXH_PRIME3;
        p += 4;
    }

    while (p < end) {
        h64 ^= static_cast<u64>(*p) * detail::XXH_PRIME5;
        h64 = detail::rotl64(h64, 11) * detail::XXH_PRIME1;
        ++p;
    }

    h64 ^= h64 >> 33;
    h64 *= detail::XXH_PRIME2;
    h64 ^= h64 >> 29;
    h64 *= detail::XXH_PRIME3;
    h64 ^= h64 >> 32;

    return h64;
}

} // namespace nyx
