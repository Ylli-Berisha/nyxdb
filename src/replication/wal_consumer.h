#pragma once

#include "common/result.h"
#include "common/types.h"
#include "replication/replication_config.h"
#include "server/wire.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace nyx {
class Database;
}

namespace nyx::replication {

class QuicClient;

class WalConsumer {
  public:
    WalConsumer(Database* db, NodeConfig config, std::string data_root);
    ~WalConsumer();

    WalConsumer(const WalConsumer&) = delete;
    WalConsumer& operator=(const WalConsumer&) = delete;

    void start();
    void stop();

    void set_leader_addr(std::string addr);

    u64 confirmed_lsn() const { return confirmed_lsn_.load(); }
    void increment_active_queries() { active_queries_.fetch_add(1); }
    void decrement_active_queries() { active_queries_.fetch_sub(1); }

  private:
    struct RespFrame {
        server::FrameType type;
        std::vector<byte> data;
    };

    void loop_();
    void load_state_();
    void persist_state_();
    Result<void> connect_and_auth_(QuicClient& client);
    void poll_loop_(QuicClient& client);
    Result<void> apply_batch_(const byte* data, usize len, u64 from_offset);
    Result<void> snapshot_resync_(QuicClient& client);

    void on_frame_(server::FrameType type, u32 qid, const byte* data, usize len);
    std::optional<RespFrame> wait_frame_(std::initializer_list<server::FrameType> expected,
                                         std::chrono::seconds timeout = std::chrono::seconds(30));

    Database* db_;
    NodeConfig config_;
    std::string data_root_;

    std::atomic<u64> confirmed_lsn_{0};
    std::atomic<u32> active_queries_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex leader_mu_;
    std::string leader_addr_;

    std::mutex resp_mu_;
    std::condition_variable resp_cv_;
    std::queue<RespFrame> resp_queue_;
};

} // namespace nyx::replication
