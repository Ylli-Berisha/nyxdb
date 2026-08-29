#pragma once

#include "common/types.h"
#include "storage/disk/type_id.h"

#include <variant>

namespace nyx {

using Value = std::variant<std::monostate, i32, i64, f64>;

inline bool is_null(const Value& v) {
    return std::holds_alternative<std::monostate>(v);
}

} // namespace nyx
