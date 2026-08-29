#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/type_id.h"

#include <string>
#include <vector>

namespace nyx {

struct Column {
    std::string name;
    TypeId type;
    bool nullable;
};

using Schema = std::vector<Column>;

namespace SchemaFile {

Result<void> write(const std::string& path, const Schema& schema);
Result<Schema> read(const std::string& path);

} // namespace SchemaFile

} // namespace nyx
