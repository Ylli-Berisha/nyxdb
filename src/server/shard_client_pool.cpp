#include "server/shard_client_pool.h"

namespace nyx::server {

Result<ExecuteResult> ShardClientPool::execute(const std::string& addr, const std::string& token,
                                               const std::string& sql) {
    Entry* entry;
    {
        std::lock_guard<std::mutex> lk(pool_mu_);
        auto it = pool_.find(addr);
        if (it == pool_.end()) {
            auto [ins, ok] = pool_.emplace(addr, std::make_unique<Entry>());
            (void)ok;
            entry = ins->second.get();
        } else {
            entry = it->second.get();
        }
    }

    std::lock_guard<std::mutex> lk(entry->mu);

    if (!entry->client.has_value()) {
        auto r = ShardClient::connect(addr, token);
        if (!r.is_ok())
            return Result<ExecuteResult>::err(r.error().message);
        entry->client = std::move(r.value());
    }

    auto result = entry->client->execute(sql);
    if (result.is_ok())
        return result;

    auto recon = entry->client->reconnect();
    if (!recon.is_ok()) {
        entry->client.reset();
        return Result<ExecuteResult>::err(result.error().message);
    }

    auto retry = entry->client->execute(sql);
    if (!retry.is_ok())
        entry->client.reset();
    return retry;
}

} // namespace nyx::server
