#pragma once

#include "common/result.h"
#include "common/types.h"
#include "database/database.h"
#include "replication/election.h"
#include "replication/follower_registry.h"
#include "replication/replication_config.h"
#include "replication/wal_consumer.h"
#include "replication/wal_streamer.h"
#include "server/wire.h"
#include "storage/wal/wal_record.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nyx::replication {

class ReplicationManager {
  public:
    static std::unique_ptr<ReplicationManager> create(NodeConfig config, Database* db,
                                                      std::string data_root);
    ~ReplicationManager();

    ReplicationManager(const ReplicationManager&) = delete;
    ReplicationManager& operator=(const ReplicationManager&) = delete;

    void start();
    void stop();

    bool is_leader() const;

    void on_hello(const std::string& node_id, u64 confirmed_lsn);

    void handle_repl_frame(const std::string& node_id, server::FrameType type, const byte* payload,
                           usize len,
                           std::function<void(server::FrameType, const std::vector<byte>&)> reply);

    Result<ExecuteResult> forward_write(const std::string& sql);

    u64 safe_compaction_lsn() const;

  private:
    ReplicationManager(NodeConfig config, Database* db, std::string data_root);

    void become_leader_();
    void become_follower_();

    std::string get_leader_addr_() const;
    std::string peer_addr_for_(const std::string& node_id) const;

    NodeConfig config_;
    Database* db_;
    std::string data_root_;

    std::unique_ptr<FollowerRegistry> registry_;
    std::unique_ptr<WalStreamer> streamer_;
    std::unique_ptr<WalConsumer> consumer_;
    std::unique_ptr<ElectionManager> election_;

    std::atomic<bool> leader_{false};
    mutable std::mutex role_mu_;
};

} // namespace nyx::replication
