#pragma once

#include "common/types.h"

namespace nyx {

enum class TypeId : u8 {
    INVALID = 0,
    INT32 = 1,
    INT64 = 2,
    DOUBLE = 3,
    VARCHAR = 4,
};

inline usize type_size(TypeId t, u16 max_len = 0) {
    switch (t) {
    case TypeId::INT32:
        return 4;
    case TypeId::INT64:
        return 8;
    case TypeId::DOUBLE:
        return 8;
    case TypeId::VARCHAR:
        return max_len > 0 ? static_cast<usize>(max_len) + 2 : 0;
    default:
        return 0;
    }
}

} // namespace nyx
