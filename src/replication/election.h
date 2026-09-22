#pragma once

#include "common/result.h"
#include "common/types.h"
#include "replication/replication_config.h"
#include "server/wire.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace nyx::replication {

class ElectionManager {
  public:
    ElectionManager(NodeConfig config, std::function<u64()> get_wal_lsn,
                    std::function<void()> on_became_leader,
                    std::function<void()> on_became_follower, std::string state_dir);
    ~ElectionManager();

    ElectionManager(const ElectionManager&) = delete;
    ElectionManager& operator=(const ElectionManager&) = delete;

    void start();
    void stop();

    void on_heartbeat(u64 term, const std::string& leader_id);
    void on_vote_req(u64 term, const std::string& candidate, u64 candidate_lsn,
                     std::function<void(bool)> respond);
    void on_vote_resp(u64 term, bool granted);

    bool is_leader() const;
    u64 current_term() const;
    std::string current_leader_id() const;

    void set_send_fn(std::function<void(const std::string& peer_addr, server::FrameType,
                                        const std::vector<byte>&)>
                         fn);

  private:
    enum class Role { Follower, Candidate, Leader };

    enum class MsgTag { Heartbeat, VoteReq, VoteResp };
    struct Msg {
        MsgTag tag;
        u64 term = 0;
        std::string from;
        u64 wal_lsn = 0;
        bool granted = false;
        std::function<void(bool)> respond;
    };

    void loop_();
    void process_msg_(Msg& msg);
    void handle_heartbeat_(const Msg& msg);
    void handle_vote_req_(Msg& msg);
    void handle_vote_resp_(const Msg& msg);

    void become_leader_();
    void become_follower_(u64 new_term, const std::string& leader_id);
    void start_election_();
    void send_heartbeats_();
    void broadcast_vote_req_();

    void persist_state_(u64 term, const std::string& voted_for);
    void load_state_();

    std::chrono::milliseconds election_timeout_() const;
    void send_to_peers_(server::FrameType type, const std::vector<byte>& payload);
    void fire_and_forget_(std::string addr, server::FrameType type, std::vector<byte> payload);

    NodeConfig config_;
    std::function<u64()> get_wal_lsn_;
    std::function<void()> on_became_leader_;
    std::function<void()> on_became_follower_;
    std::string state_dir_;
    std::vector<std::string> non_self_peers_;
    u32 cluster_size_ = 1;

    mutable std::mutex state_mu_;
    u64 current_term_ = 0;
    std::string voted_for_;
    std::string current_leader_;
    Role role_ = Role::Follower;
    u32 votes_received_ = 0;

    std::mutex msg_mu_;
    std::condition_variable msg_cv_;
    std::queue<Msg> msg_queue_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex send_fn_mu_;
    std::function<void(const std::string&, server::FrameType, const std::vector<byte>&)> send_fn_;
};

} // namespace nyx::replication
