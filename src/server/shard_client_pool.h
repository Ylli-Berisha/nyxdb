#pragma once

#include "common/result.h"
#include "database/database.h"
#include "server/shard_client.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace nyx::server {

class ShardClientPool {
  public:
    Result<ExecuteResult> execute(const std::string& addr, const std::string& token,
                                  const std::string& sql);

  private:
    struct Entry {
        std::mutex mu;
        std::optional<ShardClient> client;
    };

    std::mutex pool_mu_;
    std::unordered_map<std::string, std::unique_ptr<Entry>> pool_;
};

} // namespace nyx::server
