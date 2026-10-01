#include "server/replica_coordinator.h"

#include <algorithm>
#include <cctype>

namespace nyx::server {

ReplicaCoordinator::ReplicaCoordinator(std::string token)
    : token_(std::move(token)), pool_(std::make_unique<ShardClientPool>()) {}

ReplicaCoordinator::ReplicaCoordinator(std::string token, std::unique_ptr<ShardClientPool> pool)
    : token_(std::move(token)), pool_(std::move(pool)) {}

void ReplicaCoordinator::register_node(const std::string& addr, bool is_leader) {
    std::lock_guard<std::mutex> lk(mu_);
    if (std::find(replicas_.begin(), replicas_.end(), addr) == replicas_.end())
        replicas_.push_back(addr);
    if (is_leader)
        leader_addr_ = addr;
}

void ReplicaCoordinator::notify_leader(const std::string& old_addr, const std::string& new_addr) {
    std::lock_guard<std::mutex> lk(mu_);
    leader_addr_ = new_addr;
    auto it = std::find(replicas_.begin(), replicas_.end(), old_addr);
    if (it != replicas_.end())
        replicas_.erase(it);
    pool_->evict(old_addr);
}

bool ReplicaCoordinator::is_write_(const std::string& sql) const {
    usize i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i])))
        ++i;
    if (i + 6 > sql.size())
        return true;
    std::string first6 = sql.substr(i, 6);
    for (char& c : first6)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return first6 != "SELECT";
}

std::string ReplicaCoordinator::pick_replica_() {
    std::lock_guard<std::mutex> lk(mu_);
    if (replicas_.empty())
        return {};
    std::string addr = replicas_[rr_idx_ % replicas_.size()];
    rr_idx_ = (rr_idx_ + 1) % replicas_.size();
    return addr;
}

Result<ExecuteResult> ReplicaCoordinator::execute(const std::string& sql) {
    if (is_write_(sql)) {
        std::string leader;
        {
            std::lock_guard<std::mutex> lk(mu_);
            leader = leader_addr_;
        }
        if (leader.empty())
            return Result<ExecuteResult>::err("replica_coordinator: no leader registered");
        return pool_->execute(leader, token_, sql);
    }

    std::string replica = pick_replica_();
    if (replica.empty())
        return Result<ExecuteResult>::err("replica_coordinator: no replicas registered");
    return pool_->execute(replica, token_, sql);
}

} // namespace nyx::server
