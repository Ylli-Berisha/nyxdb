#include "replication/follower_registry.h"

#include <limits>

namespace nyx::replication {

void FollowerRegistry::upsert(const std::string& node_id, u64 confirmed_lsn, u32 active_queries) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& s = followers_[node_id];
    s.node_id = node_id;
    s.confirmed_lsn = confirmed_lsn;
    s.active_queries = active_queries;
    s.last_ack = std::chrono::steady_clock::now();
    s.stale = false;
}

void FollowerRegistry::evict_stale(u64 leader_lsn, u64 max_lag_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, s] : followers_) {
        if (!s.stale && leader_lsn > s.confirmed_lsn &&
            leader_lsn - s.confirmed_lsn > max_lag_bytes)
            s.stale = true;
    }
}

u64 FollowerRegistry::min_confirmed_lsn() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (followers_.empty())
        return std::numeric_limits<u64>::max();
    u64 min = std::numeric_limits<u64>::max();
    for (const auto& [id, s] : followers_) {
        if (!s.stale)
            min = std::min(min, s.confirmed_lsn);
    }
    return min;
}

std::vector<FollowerState> FollowerRegistry::all() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<FollowerState> out;
    out.reserve(followers_.size());
    for (const auto& [id, s] : followers_)
        out.push_back(s);
    return out;
}

} // namespace nyx::replication
