#include "replication/replication_manager.h"

#include "common/logger.h"
#include "replication/quic_client.h"
#include "replication/snapshot.h"
#include "storage/wal/wal_record.h"

#include <thread>

namespace nyx::replication {

ReplicationManager::ReplicationManager(NodeConfig config, Database* db, std::string data_root)
    : config_(std::move(config)), db_(db), data_root_(std::move(data_root)) {}

ReplicationManager::~ReplicationManager() {
    stop();
}

std::unique_ptr<ReplicationManager> ReplicationManager::create(NodeConfig config, Database* db,
                                                               std::string data_root) {
    auto* rm = new ReplicationManager(std::move(config), db, std::move(data_root));
    return std::unique_ptr<ReplicationManager>(rm);
}

void ReplicationManager::start() {
    using Role = NodeConfig::Role;

    if (config_.role == Role::Standalone)
        return;

    registry_ = std::make_unique<FollowerRegistry>();
    streamer_ = std::make_unique<WalStreamer>(db_->wal_path());

    consumer_ = std::make_unique<WalConsumer>(db_, config_, data_root_);
    if (config_.role == Role::Follower)
        consumer_->set_leader_addr(config_.leader_addr);

    election_ = std::make_unique<ElectionManager>(
        config_, [this]() { return db_->current_wal_lsn(); }, [this]() { become_leader_(); },
        [this]() { become_follower_(); }, data_root_);

    if (config_.role == Role::Leader) {
        leader_.store(true);
    } else {
        consumer_->start();
    }

    election_->start();
}

void ReplicationManager::stop() {
    if (election_)
        election_->stop();
    if (consumer_)
        consumer_->stop();
}

void ReplicationManager::become_leader_() {
    std::lock_guard<std::mutex> lk(role_mu_);
    if (leader_.load())
        return;

    if (consumer_)
        consumer_->stop();
    leader_.store(true);

    spdlog::info("repl_mgr: became leader");
}

void ReplicationManager::become_follower_() {
    std::lock_guard<std::mutex> lk(role_mu_);
    if (!leader_.load())
        return;

    leader_.store(false);

    if (consumer_ && election_) {
        std::string leader_id = election_->current_leader_id();
        std::string addr = peer_addr_for_(leader_id);
        if (!addr.empty())
            consumer_->set_leader_addr(addr);
    }

    if (consumer_)
        consumer_->start();

    spdlog::info("repl_mgr: became follower");
}

bool ReplicationManager::is_leader() const {
    if (config_.role == NodeConfig::Role::Standalone)
        return true;
    return leader_.load();
}

u64 ReplicationManager::safe_compaction_lsn() const {
    if (config_.role == NodeConfig::Role::Standalone)
        return UINT64_MAX;
    if (is_leader() && registry_)
        return registry_->min_confirmed_lsn();
    return UINT64_MAX;
}

void ReplicationManager::on_hello(const std::string& node_id, u64 confirmed_lsn) {
    if (registry_)
        registry_->upsert(node_id, confirmed_lsn, 0);
}

void ReplicationManager::handle_repl_frame(
    const std::string& node_id, server::FrameType type, const byte* payload, usize len,
    std::function<void(server::FrameType, const std::vector<byte>&)> reply) {

    if (type == server::FrameType::REPL_HEARTBEAT) {
        if (!election_ || len < 8)
            return;
        u64 term = server::decode_u64(payload);
        u16 nlen = (len >= 10) ? server::decode_u16(payload + 8) : 0;
        std::string leader_id;
        if (nlen > 0 && len >= static_cast<usize>(10 + nlen))
            leader_id.assign(reinterpret_cast<const char*>(payload + 10), nlen);
        election_->on_heartbeat(term, leader_id);
        return;
    }

    if (type == server::FrameType::REPL_VOTE_REQ) {
        if (!election_ || len < 8)
            return;
        u64 term = server::decode_u64(payload);
        if (len < 10)
            return;
        u16 nlen = server::decode_u16(payload + 8);
        if (len < static_cast<usize>(10 + nlen + 8))
            return;
        std::string candidate(reinterpret_cast<const char*>(payload + 10), nlen);
        u64 lsn = server::decode_u64(payload + 10 + nlen);

        election_->on_vote_req(term, candidate, lsn, [reply](bool granted) {
            std::vector<byte> p;
            server::encode_u8(p, granted ? 1u : 0u);
            reply(server::FrameType::REPL_VOTE_RESP, p);
        });
        return;
    }

    if (type == server::FrameType::REPL_VOTE_RESP) {
        if (!election_ || len < 9)
            return;
        u64 term = server::decode_u64(payload);
        bool granted = (payload[8] != 0);
        election_->on_vote_resp(term, granted);
        return;
    }

    if (!is_leader())
        return;

    switch (type) {
    case server::FrameType::REPL_WAL_PULL: {
        if (len < 8)
            return;
        u64 from_offset = server::decode_u64(payload);

        u64 wal_size = streamer_ ? streamer_->current_size() : 0;
        bool is_stale = (from_offset == 0) || (wal_size > 0 && from_offset > wal_size);
        if (is_stale) {
            reply(server::FrameType::REPL_WAL_STALE, {});
            return;
        }

        auto batch_r = streamer_->get_batch(from_offset);
        std::vector<byte> resp;
        server::encode_u64(resp, from_offset);
        if (batch_r.is_ok())
            resp.insert(resp.end(), batch_r.value().begin(), batch_r.value().end());
        reply(server::FrameType::REPL_WAL_BATCH, resp);
        break;
    }

    case server::FrameType::REPL_WAL_ACK: {
        if (len < 2)
            return;
        u16 nlen = server::decode_u16(payload);
        if (len < static_cast<usize>(2 + nlen + 12))
            return;
        std::string nid(reinterpret_cast<const char*>(payload + 2), nlen);
        u64 confirmed = server::decode_u64(payload + 2 + nlen);
        u32 active = server::decode_u32(payload + 2 + nlen + 8);

        if (registry_) {
            registry_->upsert(nid, confirmed, active);
            registry_->evict_stale(db_->current_wal_lsn(), config_.max_wal_lag_bytes);
        }
        break;
    }

    case server::FrameType::REPL_SNAPSHOT_REQ: {
        SnapshotSender sender(db_, data_root_);
        sender.send(0, [&reply](server::FrameType t, const std::vector<byte>& p) {
            reply(t, p);
            return Result<void>::ok();
        });
        break;
    }

    case server::FrameType::REPL_FORWARD_WRITE: {
        if (len < 2)
            return;
        u16 slen = server::decode_u16(payload);
        if (len < static_cast<usize>(2 + slen))
            return;
        std::string sql(reinterpret_cast<const char*>(payload + 2), slen);

        auto r = db_->execute(sql);
        std::vector<byte> resp;
        if (r.is_ok()) {
            server::encode_u8(resp, 0u);
            server::encode_u64(resp, r.value().rows_affected);
        } else {
            server::encode_u8(resp, 1u);
            server::encode_str(resp, r.error().message);
        }
        reply(server::FrameType::REPL_FORWARD_RESP, resp);
        break;
    }

    default:
        break;
    }
}

Result<ExecuteResult> ReplicationManager::forward_write(const std::string& sql) {
    struct FrameQ {
        std::mutex mu;
        std::condition_variable cv;
        std::queue<std::pair<server::FrameType, std::vector<byte>>> q;

        std::optional<std::pair<server::FrameType, std::vector<byte>>>
        wait(std::chrono::seconds t) {
            std::unique_lock<std::mutex> lk(mu);
            if (!cv.wait_for(lk, t, [this] { return !q.empty(); }))
                return std::nullopt;
            auto fr = std::move(q.front());
            q.pop();
            return fr;
        }
    };

    auto fq = std::make_shared<FrameQ>();

    auto cr = QuicClient::create();
    if (!cr.is_ok())
        return Result<ExecuteResult>::err(cr.error().message);

    auto* client_ptr = cr.value().get();
    client_ptr->set_frame_handler([fq](server::FrameType t, u32, const byte* d, usize l) {
        std::lock_guard<std::mutex> lk(fq->mu);
        fq->q.push({t, std::vector<byte>(d, d + l)});
        fq->cv.notify_one();
    });

    std::string addr = get_leader_addr_();
    auto [host, port] = parse_node_addr(addr);
    if (client_ptr->connect(host, port).is_err())
        return Result<ExecuteResult>::err("forward_write: connect to leader failed");

    std::vector<byte> auth_payload;
    server::encode_str(auth_payload, config_.auth_token);
    client_ptr->send_frame(server::FrameType::AUTH_REQ, 1, auth_payload);

    auto auth_r = fq->wait(std::chrono::seconds(5));
    if (!auth_r || auth_r->first != server::FrameType::AUTH_OK)
        return Result<ExecuteResult>::err("forward_write: auth failed");

    std::vector<byte> fwd;
    server::encode_str(fwd, sql);
    client_ptr->send_frame(server::FrameType::REPL_FORWARD_WRITE, 0, fwd);

    auto resp = fq->wait(std::chrono::seconds(10));
    client_ptr->close();

    if (!resp || resp->first != server::FrameType::REPL_FORWARD_RESP)
        return Result<ExecuteResult>::err("forward_write: no response from leader");

    const byte* d = resp->second.data();
    usize l = resp->second.size();
    if (l < 1)
        return Result<ExecuteResult>::err("forward_write: empty response");

    if (d[0] == 0) {
        u64 rows = (l >= 9) ? server::decode_u64(d + 1) : 0;
        ExecuteResult r;
        r.rows_affected = rows;
        return Result<ExecuteResult>::ok(std::move(r));
    }
    if (l < 3)
        return Result<ExecuteResult>::err("forward_write: error response truncated");
    u16 mlen = server::decode_u16(d + 1);
    return Result<ExecuteResult>::err(
        std::string(reinterpret_cast<const char*>(d + 3), std::min((usize)mlen, l - 3)));
}

std::string ReplicationManager::get_leader_addr_() const {
    if (election_) {
        std::string id = election_->current_leader_id();
        if (!id.empty()) {
            auto a = peer_addr_for_(id);
            if (!a.empty())
                return a;
        }
    }
    return config_.leader_addr;
}

std::string ReplicationManager::peer_addr_for_(const std::string& node_id) const {
    for (const auto& addr : config_.peer_addrs) {
        auto [host, port] = parse_node_addr(addr);
        if (host == node_id)
            return addr;
    }
    return {};
}

} // namespace nyx::replication
