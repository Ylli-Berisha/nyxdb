#pragma once

#include "binder/bound_ast.h"
#include "catalog/catalog.h"
#include "common/result.h"
#include "database/database.h"
#include "server/shard_client_pool.h"

#include <memory>
#include <string>

namespace nyx::server {

class Coordinator {
  public:
    static Result<Coordinator> open(const std::string& data_root, std::string token);

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
    Coordinator(Coordinator&&) noexcept = default;
    Coordinator& operator=(Coordinator&&) noexcept = default;

    Result<ExecuteResult> execute(const std::string& sql);

  private:
    explicit Coordinator(Catalog cat, std::string token);

    Result<ExecuteResult> route_insert_(const bound::BoundInsert& stmt, const std::string& sql);
    Result<ExecuteResult> route_delete_(const bound::BoundDelete& stmt, const std::string& sql);
    Result<ExecuteResult> route_update_(const bound::BoundUpdate& stmt, const std::string& sql);
    Result<ExecuteResult> fan_out_select_(const bound::BoundSelect& stmt, const std::string& sql,
                                          const ShardMapMeta& sm);
    Result<ExecuteResult> handle_alter_partition_(const bound::BoundAlterAddPartition& stmt);

    Catalog catalog_;
    std::string token_;
    std::unique_ptr<ShardClientPool> pool_;
};

} // namespace nyx::server
