#pragma once

#include "common/result.h"
#include "database/database.h"
#include "replication/quic_client.h"

#include <memory>
#include <string>

namespace nyx::server {

class ShardClient {
  public:
    static Result<ShardClient> connect(const std::string& addr, const std::string& token);

    Result<ExecuteResult> execute(const std::string& sql);
    Result<void> reconnect();

    ShardClient(ShardClient&&) noexcept = default;
    ShardClient& operator=(ShardClient&&) noexcept = default;
    ShardClient(const ShardClient&) = delete;
    ShardClient& operator=(const ShardClient&) = delete;

  private:
    explicit ShardClient(std::unique_ptr<replication::QuicClient> conn, std::string addr,
                         std::string token);

    std::unique_ptr<replication::QuicClient> conn_;
    std::string addr_;
    std::string token_;
};

} // namespace nyx::server
