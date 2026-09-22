#pragma once

#include "common/types.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nyx::replication {

struct NodeConfig {
    std::string node_id;
    enum class Role { Standalone, Leader, Follower } role = Role::Standalone;
    std::vector<std::string> peer_addrs;
    u16 port = 4433;
    u64 max_wal_lag_bytes = 4ULL * 1024 * 1024 * 1024;
    std::string auth_token;
    std::string leader_addr;
    u32 election_timeout_min_ms = 3000;
    u32 election_timeout_max_ms = 5000;
    u32 heartbeat_interval_ms = 1000;
};

inline std::pair<std::string, u16> parse_node_addr(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos || colon + 1 >= addr.size())
        return {addr, 4433};
    u16 port = 4433;
    try {
        port = static_cast<u16>(std::stoi(addr.substr(colon + 1)));
    } catch (...) {
    }
    return {addr.substr(0, colon), port};
}

} // namespace nyx::replication
