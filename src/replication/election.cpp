#include "replication/election.h"

#include "common/logger.h"
#include "replication/quic_client.h"
#include "storage/wal/wal_record.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <unistd.h>

namespace nyx::replication {

static std::pair<std::string, u16> parse_addr(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos || colon + 1 >= addr.size())
        return {addr, 4433};
    u16 port = 4433;
    try {
        port = static_cast<u16>(std::stoi(addr.substr(colon + 1)));
    } catch (...) {
    }
    return {addr.substr(0, colon), port};
}

ElectionManager::ElectionManager(NodeConfig config, std::function<u64()> get_wal_lsn,
                                 std::function<void()> on_became_leader,
                                 std::function<void()> on_became_follower, std::string state_dir)
    : config_(std::move(config)), get_wal_lsn_(std::move(get_wal_lsn)),
      on_became_leader_(std::move(on_became_leader)),
      on_became_follower_(std::move(on_became_follower)), state_dir_(std::move(state_dir)) {
    for (const auto& peer : config_.peer_addrs) {
        auto [host, port] = parse_addr(peer);
        if (host != config_.node_id)
            non_self_peers_.push_back(peer);
    }
    cluster_size_ = static_cast<u32>(config_.peer_addrs.size());
    if (cluster_size_ == 0)
        cluster_size_ = 1;

    send_fn_ = [this](const std::string& addr, server::FrameType type,
                      const std::vector<byte>& payload) { fire_and_forget_(addr, type, payload); };
}

ElectionManager::~ElectionManager() {
    stop();
}

void ElectionManager::start() {
    load_state_();
    if (config_.role == NodeConfig::Role::Leader) {
        std::lock_guard<std::mutex> lk(state_mu_);
        role_ = Role::Leader;
        if (current_term_ == 0)
            current_term_ = 1;
        current_leader_ = config_.node_id;
    }
    running_.store(true);
    thread_ = std::thread(&ElectionManager::loop_, this);
}

void ElectionManager::stop() {
    running_.store(false);
    msg_cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void ElectionManager::set_send_fn(
    std::function<void(const std::string&, server::FrameType, const std::vector<byte>&)> fn) {
    std::lock_guard<std::mutex> lk(send_fn_mu_);
    send_fn_ = std::move(fn);
}

void ElectionManager::on_heartbeat(u64 term, const std::string& leader_id) {
    Msg m;
    m.tag = MsgTag::Heartbeat;
    m.term = term;
    m.from = leader_id;
    std::lock_guard<std::mutex> lk(msg_mu_);
    msg_queue_.push(std::move(m));
    msg_cv_.notify_one();
}

void ElectionManager::on_vote_req(u64 term, const std::string& candidate, u64 candidate_lsn,
                                  std::function<void(bool)> respond) {
    Msg m;
    m.tag = MsgTag::VoteReq;
    m.term = term;
    m.from = candidate;
    m.wal_lsn = candidate_lsn;
    m.respond = std::move(respond);
    std::lock_guard<std::mutex> lk(msg_mu_);
    msg_queue_.push(std::move(m));
    msg_cv_.notify_one();
}

void ElectionManager::on_vote_resp(u64 term, bool granted) {
    Msg m;
    m.tag = MsgTag::VoteResp;
    m.term = term;
    m.granted = granted;
    std::lock_guard<std::mutex> lk(msg_mu_);
    msg_queue_.push(std::move(m));
    msg_cv_.notify_one();
}

bool ElectionManager::is_leader() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return role_ == Role::Leader;
}

u64 ElectionManager::current_term() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return current_term_;
}

std::string ElectionManager::current_leader_id() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return current_leader_;
}

void ElectionManager::loop_() {
    while (running_) {
        Role cur_role;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            cur_role = role_;
        }

        auto timeout = (cur_role == Role::Leader)
                           ? std::chrono::milliseconds(config_.heartbeat_interval_ms)
                           : election_timeout_();

        std::vector<Msg> batch;
        {
            std::unique_lock<std::mutex> lk(msg_mu_);
            msg_cv_.wait_for(lk, timeout, [&] { return !msg_queue_.empty() || !running_; });
            if (!running_)
                break;

            while (!msg_queue_.empty()) {
                batch.push_back(std::move(msg_queue_.front()));
                msg_queue_.pop();
            }
        }

        if (batch.empty()) {
            Role r;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                r = role_;
            }
            if (r == Role::Leader)
                send_heartbeats_();
            else
                start_election_();
        } else {
            for (auto& msg : batch)
                process_msg_(msg);
        }
    }
}

void ElectionManager::process_msg_(Msg& msg) {
    switch (msg.tag) {
    case MsgTag::Heartbeat:
        handle_heartbeat_(msg);
        break;
    case MsgTag::VoteReq:
        handle_vote_req_(msg);
        break;
    case MsgTag::VoteResp:
        handle_vote_resp_(msg);
        break;
    }
}

void ElectionManager::handle_heartbeat_(const Msg& msg) {
    bool became_follower = false;
    u64 persist_term = 0;

    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (msg.term < current_term_)
            return;

        if (msg.term > current_term_) {
            current_term_ = msg.term;
            voted_for_.clear();
            persist_term = current_term_;
            if (role_ != Role::Follower) {
                role_ = Role::Follower;
                became_follower = true;
            }
            current_leader_ = msg.from;
        } else if (role_ == Role::Candidate) {
            role_ = Role::Follower;
            became_follower = true;
            current_leader_ = msg.from;
        } else if (role_ == Role::Follower) {
            current_leader_ = msg.from;
        } else if (role_ == Role::Leader && msg.from != config_.node_id) {
            role_ = Role::Follower;
            became_follower = true;
            current_leader_ = msg.from;
            persist_term = current_term_;
        }
    }

    if (persist_term > 0)
        persist_state_(persist_term, "");

    if (became_follower && on_became_follower_) {
        spdlog::info("election: stepped down to follower (term {})", msg.term);
        on_became_follower_();
    }
}

void ElectionManager::handle_vote_req_(Msg& msg) {
    u64 my_lsn = get_wal_lsn_();

    bool grant = false;
    bool became_follower = false;
    u64 persist_term = 0;
    std::string persist_voted;

    {
        std::lock_guard<std::mutex> lk(state_mu_);

        if (msg.term > current_term_) {
            current_term_ = msg.term;
            voted_for_.clear();
            if (role_ != Role::Follower) {
                role_ = Role::Follower;
                became_follower = true;
            }
        }

        if (msg.term >= current_term_ && msg.wal_lsn >= my_lsn &&
            (voted_for_.empty() || voted_for_ == msg.from)) {
            grant = true;
            voted_for_ = msg.from;
            persist_term = current_term_;
            persist_voted = voted_for_;
        }
    }

    if (grant)
        persist_state_(persist_term, persist_voted);
    if (became_follower && on_became_follower_)
        on_became_follower_();

    spdlog::debug("election: vote_req from {} term {} lsn {} → {}", msg.from, msg.term, msg.wal_lsn,
                  grant ? "grant" : "deny");

    msg.respond(grant);
}

void ElectionManager::handle_vote_resp_(const Msg& msg) {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (msg.term > current_term_) {
            current_term_ = msg.term;
            voted_for_.clear();
            role_ = Role::Follower;
            current_leader_.clear();
        }

        if (role_ != Role::Candidate || msg.term < current_term_)
            return;
        if (!msg.granted)
            return;

        ++votes_received_;

        u32 majority = (cluster_size_ + 1) / 2;
        if (votes_received_ < majority)
            return;
    }

    become_leader_();
}

void ElectionManager::become_leader_() {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        role_ = Role::Leader;
        current_leader_ = config_.node_id;
        votes_received_ = 0;
    }

    spdlog::info("election: became leader (term {})", current_term());

    if (on_became_leader_)
        on_became_leader_();
    send_heartbeats_();
}

void ElectionManager::become_follower_(u64 new_term, const std::string& leader_id) {
    bool was_not_follower;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        was_not_follower = (role_ != Role::Follower);
        current_term_ = new_term;
        voted_for_.clear();
        role_ = Role::Follower;
        current_leader_ = leader_id;
        votes_received_ = 0;
    }

    persist_state_(new_term, "");

    if (was_not_follower && on_became_follower_)
        on_became_follower_();
}

void ElectionManager::start_election_() {
    u64 new_term;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        ++current_term_;
        voted_for_ = config_.node_id;
        votes_received_ = 1;
        role_ = Role::Candidate;
        new_term = current_term_;
    }

    spdlog::info("election: starting election for term {}", new_term);

    persist_state_(new_term, config_.node_id);
    broadcast_vote_req_();

    u32 majority = (cluster_size_ + 1) / 2;
    if (1 >= majority)
        become_leader_();
}

void ElectionManager::send_heartbeats_() {
    if (non_self_peers_.empty())
        return;

    u64 term;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (role_ != Role::Leader)
            return;
        term = current_term_;
    }

    std::vector<byte> payload;
    server::encode_u64(payload, term);
    server::encode_str(payload, config_.node_id);
    send_to_peers_(server::FrameType::REPL_HEARTBEAT, payload);
}

void ElectionManager::broadcast_vote_req_() {
    if (non_self_peers_.empty())
        return;

    u64 term;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        term = current_term_;
    }
    u64 lsn = get_wal_lsn_();

    std::vector<byte> payload;
    server::encode_u64(payload, term);
    server::encode_str(payload, config_.node_id);
    server::encode_u64(payload, lsn);
    send_to_peers_(server::FrameType::REPL_VOTE_REQ, payload);
}

void ElectionManager::send_to_peers_(server::FrameType type, const std::vector<byte>& payload) {
    std::function<void(const std::string&, server::FrameType, const std::vector<byte>&)> fn;
    {
        std::lock_guard<std::mutex> lk(send_fn_mu_);
        fn = send_fn_;
    }
    for (const auto& peer : non_self_peers_)
        fn(peer, type, payload);
}

void ElectionManager::fire_and_forget_(std::string addr, server::FrameType type,
                                       std::vector<byte> payload) {
    std::string auth = config_.auth_token;
    std::string node_id = config_.node_id;

    std::thread([addr = std::move(addr), type, payload = std::move(payload), auth = std::move(auth),
                 node_id = std::move(node_id)]() {
        struct FrameQ {
            std::mutex mu;
            std::condition_variable cv;
            std::queue<std::pair<server::FrameType, std::vector<byte>>> q;
            std::optional<server::FrameType> wait_type(std::chrono::seconds t) {
                std::unique_lock<std::mutex> lk(mu);
                if (!cv.wait_for(lk, t, [this] { return !q.empty(); }))
                    return std::nullopt;
                auto fr = std::move(q.front());
                q.pop();
                return fr.first;
            }
        };

        auto fq = std::make_shared<FrameQ>();

        auto cr = QuicClient::create();
        if (!cr.is_ok())
            return;
        auto& c = *cr.value();

        c.set_frame_handler([fq](server::FrameType t, u32, const byte*, usize) {
            std::lock_guard<std::mutex> lk(fq->mu);
            fq->q.push({t, {}});
            fq->cv.notify_one();
        });

        auto [host, port] = parse_addr(addr);
        if (c.connect(host, port).is_err())
            return;

        std::vector<byte> auth_payload;
        server::encode_str(auth_payload, auth);
        if (c.send_frame(server::FrameType::AUTH_REQ, 1, auth_payload).is_err())
            return;

        auto resp = fq->wait_type(std::chrono::seconds(5));
        if (!resp || *resp != server::FrameType::AUTH_OK)
            return;

        c.send_frame(type, 0, payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        c.close();
    }).detach();
}

void ElectionManager::persist_state_(u64 term, const std::string& voted_for) {
    std::string path = state_dir_ + "/election_state.bin";
    std::string tmp = path + ".tmp";

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;

    u8 hdr[10];
    wal_put_u64(hdr, term);
    wal_put_u16(hdr + 8, static_cast<u16>(voted_for.size()));
    ::write(fd, hdr, 10);
    if (!voted_for.empty())
        ::write(fd, voted_for.data(), voted_for.size());
    ::fsync(fd);
    ::close(fd);
    ::rename(tmp.c_str(), path.c_str());
}

void ElectionManager::load_state_() {
    std::string path = state_dir_ + "/election_state.bin";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return;

    u8 hdr[10];
    if (::read(fd, hdr, 10) == 10) {
        std::lock_guard<std::mutex> lk(state_mu_);
        current_term_ = wal_read_u64(hdr);
        u16 vlen = wal_read_u16(hdr + 8);
        if (vlen > 0) {
            voted_for_.resize(vlen);
            if (::read(fd, voted_for_.data(), vlen) != vlen)
                voted_for_.clear();
        }
    }
    ::close(fd);
}

std::chrono::milliseconds ElectionManager::election_timeout_() const {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(static_cast<int>(config_.election_timeout_min_ms),
                                            static_cast<int>(config_.election_timeout_max_ms));
    return std::chrono::milliseconds(dist(rng));
}

} // namespace nyx::replication
