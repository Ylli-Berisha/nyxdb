#pragma once

#include "binder/bound_ast.h"
#include "catalog/catalog.h"
#include "common/result.h"
#include "database/database.h"
#include "server/shard_client_pool.h"

#include <memory>
#include <mutex>
#include <string>

namespace nyx::server {

class ShardCoordinator {
  public:
    static Result<ShardCoordinator> open(const std::string& data_root, std::string token);

    ShardCoordinator(const ShardCoordinator&) = delete;
    ShardCoordinator& operator=(const ShardCoordinator&) = delete;
    ShardCoordinator(ShardCoordinator&&) noexcept;
    ShardCoordinator& operator=(ShardCoordinator&&) noexcept;

    Result<ExecuteResult> execute(const std::string& sql);
    void notify_leader(const std::string& old_addr, const std::string& new_addr);

  private:
    explicit ShardCoordinator(Catalog cat, std::string token);

    Result<ExecuteResult> route_insert_(const bound::BoundInsert& stmt, const std::string& sql);
    Result<ExecuteResult> route_delete_(const bound::BoundDelete& stmt, const std::string& sql);
    Result<ExecuteResult> route_update_(const bound::BoundUpdate& stmt, const std::string& sql);
    Result<ExecuteResult> fan_out_select_(const bound::BoundSelect& stmt, const std::string& sql,
                                          const ShardMapMeta& sm);
    Result<ExecuteResult> handle_alter_partition_(const bound::BoundAlterAddPartition& stmt);

    std::mutex mu_;
    Catalog catalog_;
    std::string token_;
    std::unique_ptr<ShardClientPool> pool_;
};

} // namespace nyx::server
