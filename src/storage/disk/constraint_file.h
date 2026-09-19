#pragma once

#include "common/result.h"
#include "common/types.h"

#include <string>
#include <vector>

namespace nyx {

enum class ConstraintKind : u8 { PRIMARY_KEY = 0, UNIQUE = 1 };

struct ConstraintMeta {
    ConstraintKind kind;
    std::string name;
    std::vector<u8> col_indices;
};

namespace ConstraintFile {

Result<void> write(const std::string& path, const std::vector<ConstraintMeta>& constraints);
Result<std::vector<ConstraintMeta>> read(const std::string& path);

} // namespace ConstraintFile

} // namespace nyx
