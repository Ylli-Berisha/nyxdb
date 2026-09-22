#pragma once

#include "common/types.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nyx::replication {

struct FollowerState {
    std::string node_id;
    u64 confirmed_lsn = 0;
    u32 active_queries = 0;
    std::chrono::steady_clock::time_point last_ack;
    bool stale = false;
};

class FollowerRegistry {
  public:
    void upsert(const std::string& node_id, u64 confirmed_lsn, u32 active_queries);
    void evict_stale(u64 leader_lsn, u64 max_lag_bytes);
    u64 min_confirmed_lsn() const;
    std::vector<FollowerState> all() const;

  private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, FollowerState> followers_;
};

} // namespace nyx::replication
