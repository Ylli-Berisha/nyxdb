#pragma once

#include "common/result.h"
#include "common/types.h"
#include "database/database.h"
#include "server/shard_client_pool.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nyx::server {

class ReplicaCoordinator {
  public:
    explicit ReplicaCoordinator(std::string token);
    ReplicaCoordinator(std::string token, std::unique_ptr<ShardClientPool> pool);

    void register_node(const std::string& addr, bool is_leader);
    void notify_leader(const std::string& old_addr, const std::string& new_addr);
    Result<ExecuteResult> execute(const std::string& sql);

  private:
    bool is_write_(const std::string& sql) const;
    std::string pick_replica_();

    std::mutex mu_;
    std::vector<std::string> replicas_;
    std::string leader_addr_;
    usize rr_idx_ = 0;
    std::string token_;
    std::unique_ptr<ShardClientPool> pool_;
};

} // namespace nyx::server
