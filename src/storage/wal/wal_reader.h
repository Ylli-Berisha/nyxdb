#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/schema.h"
#include "storage/disk/value.h"

#include <string>
#include <vector>

namespace nyx {

struct WalRecord {
    enum class Type : u8 { Insert = 0x01, CreateTable = 0x02, Delete = 0x03, Update = 0x04 };

    Type type;
    u64 byte_offset;
    std::string table_name;
    Schema schema;
    std::vector<std::vector<Value>> rows;
    std::vector<u64> row_indices;
};

class WalReader {
  public:
    static Result<WalReader> open(const std::string& path);

    ~WalReader();
    WalReader(const WalReader&) = delete;
    WalReader& operator=(const WalReader&) = delete;
    WalReader(WalReader&&) noexcept;
    WalReader& operator=(WalReader&&) noexcept;

    Result<std::vector<WalRecord>> read_all();

  private:
    explicit WalReader(int fd);
    int fd_;
};

} // namespace nyx
