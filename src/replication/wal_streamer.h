#pragma once

#include "common/result.h"
#include "common/types.h"

#include <string>
#include <vector>

namespace nyx::replication {

class WalStreamer {
  public:
    explicit WalStreamer(std::string wal_path);

    Result<std::vector<byte>> get_batch(u64 from_offset, u64 max_bytes = 1ULL * 1024 * 1024) const;
    u64 current_size() const;

  private:
    std::string wal_path_;
};

} // namespace nyx::replication
