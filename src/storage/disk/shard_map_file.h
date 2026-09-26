#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/type_id.h"
#include "storage/disk/value.h"

#include <string>
#include <vector>

namespace nyx {

struct PartitionDef {
    std::string name;
    bool is_maxvalue = false;
    Value upper_bound;
    std::string node_addr;
};

struct ShardMapMeta {
    std::string partition_col;
    u8 partition_col_idx = 0;
    std::vector<PartitionDef> partitions;
};

size_t shard_for_value(const ShardMapMeta& meta, const Value& val);

std::vector<size_t> prune_partitions(const ShardMapMeta& meta, u8 predicate_col_idx, int op_kind,
                                     const Value& literal);

namespace ShardMapFile {

Result<void> write(const std::string& path, const ShardMapMeta& meta);
Result<ShardMapMeta> read(const std::string& path);

} // namespace ShardMapFile

} // namespace nyx
