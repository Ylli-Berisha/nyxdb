#pragma once

#include "common/result.h"
#include "common/types.h"
#include "storage/disk/schema.h"
#include "storage/disk/value.h"

#include <string>
#include <vector>

namespace nyx {

class WalWriter {
  public:
    static Result<WalWriter> open(const std::string& path);

    ~WalWriter();
    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;
    WalWriter(WalWriter&& other) noexcept;
    WalWriter& operator=(WalWriter&& other) noexcept;

    Result<void> log_insert(const std::string& table, const Schema& schema,
                            const std::vector<std::vector<Value>>& rows);
    Result<void> log_create_table(const std::string& table, const Schema& schema);
    Result<void> log_delete(const std::string& table, const std::vector<u64>& row_indices);
    Result<void> log_update(const std::string& table, const std::vector<u64>& old_indices,
                            const Schema& schema, const std::vector<std::vector<Value>>& new_rows);

    u64 current_offset() const { return offset_; }
    Result<void> checkpoint();

  private:
    WalWriter(int fd, u64 offset, std::string path);
    Result<void> write_record(std::vector<u8>& buf);

    int fd_;
    u64 offset_;
    std::string path_;
};

} // namespace nyx
