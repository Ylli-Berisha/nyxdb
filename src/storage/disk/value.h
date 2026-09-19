#pragma once

#include "common/types.h"
#include "storage/disk/type_id.h"

#include <string>
#include <variant>

namespace nyx {

struct Date {
    i32 days;
    bool operator==(const Date& o) const { return days == o.days; }
};

struct Timestamp {
    i64 micros;
    bool operator==(const Timestamp& o) const { return micros == o.micros; }
};

using Value = std::variant<std::monostate, i32, i64, f64, std::string, bool, Date, Timestamp>;

inline bool is_null(const Value& v) {
    return std::holds_alternative<std::monostate>(v);
}

} // namespace nyx
