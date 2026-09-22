#include "replication/wal_consumer.h"

#include "common/logger.h"
#include "database/database.h"
#include "replication/quic_client.h"
#include "replication/snapshot.h"
#include "storage/wal/wal_reader.h"
#include "storage/wal/wal_record.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace nyx::replication {

WalConsumer::WalConsumer(Database* db, NodeConfig config, std::string data_root)
    : db_(db), config_(std::move(config)), data_root_(std::move(data_root)),
      leader_addr_(config_.leader_addr) {}

WalConsumer::~WalConsumer() {
    stop();
}

void WalConsumer::start() {
    running_.store(true);
    thread_ = std::thread(&WalConsumer::loop_, this);
}

void WalConsumer::stop() {
    running_.store(false);
    resp_cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void WalConsumer::set_leader_addr(std::string addr) {
    std::lock_guard<std::mutex> lk(leader_mu_);
    leader_addr_ = std::move(addr);
}

void WalConsumer::on_frame_(server::FrameType type, u32 /*qid*/, const byte* data, usize len) {
    std::lock_guard<std::mutex> lk(resp_mu_);
    resp_queue_.push({type, std::vector<byte>(data, data + len)});
    resp_cv_.notify_one();
}

std::optional<WalConsumer::RespFrame>
WalConsumer::wait_frame_(std::initializer_list<server::FrameType> expected,
                         std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lk(resp_mu_);

    while (running_) {
        resp_cv_.wait_until(lk, deadline, [&] { return !resp_queue_.empty() || !running_; });

        if (!running_)
            return std::nullopt;
        if (resp_queue_.empty())
            return std::nullopt;

        auto fr = std::move(resp_queue_.front());
        resp_queue_.pop();

        for (auto t : expected) {
            if (fr.type == t)
                return fr;
        }
    }
    return std::nullopt;
}

void WalConsumer::load_state_() {
    std::string path = data_root_ + "/replication_state.bin";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return;

    u8 buf[16];
    if (::read(fd, buf, 16) == 16)
        confirmed_lsn_.store(wal_read_u64(buf));
    ::close(fd);
}

void WalConsumer::persist_state_() {
    std::string path = data_root_ + "/replication_state.bin";
    std::string tmp = path + ".tmp";

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;

    u8 buf[16];
    wal_put_u64(buf, confirmed_lsn_.load());
    wal_put_u64(buf + 8, 0); // term slot — populated by election (C5)

    if (::write(fd, buf, 16) == 16)
        ::fsync(fd);
    ::close(fd);
    ::rename(tmp.c_str(), path.c_str());
}

Result<void> WalConsumer::connect_and_auth_(QuicClient& client) {
    std::string host;
    u16 port;
    {
        std::lock_guard<std::mutex> lk(leader_mu_);
        auto colon = leader_addr_.rfind(':');
        if (colon == std::string::npos || colon + 1 >= leader_addr_.size()) {
            host = leader_addr_;
            port = 4433;
        } else {
            host = leader_addr_.substr(0, colon);
            try {
                port = static_cast<u16>(std::stoi(leader_addr_.substr(colon + 1)));
            } catch (...) {
                port = 4433;
            }
        }
    }

    auto cr = client.connect(host, port);
    if (cr.is_err())
        return cr;

    std::vector<byte> auth;
    server::encode_str(auth, config_.auth_token);
    auto ar = client.send_frame(server::FrameType::AUTH_REQ, 1, auth);
    if (ar.is_err())
        return ar;

    auto resp = wait_frame_({server::FrameType::AUTH_OK, server::FrameType::AUTH_ERR},
                            std::chrono::seconds(10));
    if (!resp)
        return Result<void>::err("auth: timed out waiting for leader");
    if (resp->type == server::FrameType::AUTH_ERR)
        return Result<void>::err("auth: rejected by leader");

    std::vector<byte> hello;
    server::encode_str(hello, config_.node_id);
    server::encode_u64(hello, confirmed_lsn_.load());
    return client.send_frame(server::FrameType::REPL_HELLO, 0, hello);
}

Result<void> WalConsumer::apply_batch_(const byte* data, usize len, u64 from_offset) {
    char tmp_path[] = "/tmp/nyxdb_wal_batch_XXXXXX";
    int fd = ::mkstemp(tmp_path);
    if (fd < 0)
        return Result<void>::err("apply_batch: mkstemp failed: " + std::string(strerror(errno)));

    u8 hdr[WAL_HEADER_SIZE];
    std::memcpy(hdr, WAL_MAGIC, 4);
    wal_put_u16(hdr + 4, WAL_VERSION);

    bool ok = (::write(fd, hdr, WAL_HEADER_SIZE) == static_cast<ssize_t>(WAL_HEADER_SIZE) &&
               ::write(fd, data, len) == static_cast<ssize_t>(len));
    ::close(fd);

    if (!ok) {
        ::unlink(tmp_path);
        return Result<void>::err("apply_batch: write to temp file failed");
    }

    auto rr = WalReader::open(tmp_path);
    ::unlink(tmp_path);
    if (!rr.is_ok())
        return Result<void>::err("apply_batch: " + rr.error().message);

    u64 bytes_consumed = 0;
    auto records_r = rr.value().read_all(&bytes_consumed);
    if (!records_r.is_ok())
        return Result<void>::err("apply_batch: " + records_r.error().message);

    for (const auto& rec : records_r.value()) {
        auto r = db_->apply_replicated_record(rec);
        if (r.is_err())
            return r;
    }

    confirmed_lsn_.store(from_offset + bytes_consumed);
    persist_state_();
    return Result<void>::ok();
}

Result<void> WalConsumer::snapshot_resync_(QuicClient& client) {
    std::vector<byte> req;
    server::encode_str(req, config_.node_id);
    auto sr = client.send_frame(server::FrameType::REPL_SNAPSHOT_REQ, 0, req);
    if (sr.is_err())
        return sr;

    auto meta = wait_frame_({server::FrameType::REPL_SNAPSHOT_META}, std::chrono::seconds(60));
    if (!meta)
        return Result<void>::err("snapshot: no META received");

    SnapshotReceiver recv(data_root_);
    auto r = recv.on_meta(meta->data.data(), meta->data.size());
    if (r.is_err())
        return r;

    while (running_) {
        auto frame = wait_frame_(
            {server::FrameType::REPL_SNAPSHOT_DATA, server::FrameType::REPL_SNAPSHOT_END},
            std::chrono::seconds(60));
        if (!frame)
            return Result<void>::err("snapshot: timed out receiving data");

        if (frame->type == server::FrameType::REPL_SNAPSHOT_DATA) {
            auto dr = recv.on_data(frame->data.data(), frame->data.size());
            if (dr.is_err())
                return dr;
        } else {
            auto end_r = recv.on_end(frame->data.data(), frame->data.size());
            if (!end_r.is_ok())
                return Result<void>::err(end_r.error().message);

            u64 resume_lsn = end_r.value();
            confirmed_lsn_.store(resume_lsn);
            persist_state_();

            auto reopen_r = db_->reopen();
            if (reopen_r.is_err())
                return reopen_r;

            return Result<void>::ok();
        }
    }
    return Result<void>::err("snapshot: interrupted by stop");
}

void WalConsumer::poll_loop_(QuicClient& client) {
    while (running_) {
        std::vector<byte> pull;
        server::encode_u64(pull, confirmed_lsn_.load());
        auto sr = client.send_frame(server::FrameType::REPL_WAL_PULL, 0, pull);
        if (sr.is_err())
            break;

        auto resp =
            wait_frame_({server::FrameType::REPL_WAL_BATCH, server::FrameType::REPL_WAL_STALE},
                        std::chrono::seconds(30));
        if (!resp)
            continue;

        if (resp->type == server::FrameType::REPL_WAL_STALE) {
            auto r = snapshot_resync_(client);
            if (r.is_err()) {
                spdlog::warn("wal_consumer: snapshot resync failed: {}", r.error().message);
                break;
            }
            continue;
        }

        // REPL_WAL_BATCH: payload = from_offset(u64) + raw_wal_bytes
        if (resp->data.size() < 8)
            continue;

        u64 from_offset = server::decode_u64(resp->data.data());
        const byte* batch_data = resp->data.data() + 8;
        usize batch_len = resp->data.size() - 8;

        if (batch_len == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto r = apply_batch_(batch_data, batch_len, from_offset);
        if (r.is_err()) {
            spdlog::warn("wal_consumer: apply_batch failed: {}", r.error().message);
            break;
        }

        std::vector<byte> ack;
        server::encode_str(ack, config_.node_id);
        server::encode_u64(ack, confirmed_lsn_.load());
        server::encode_u32(ack, active_queries_.load());
        client.send_frame(server::FrameType::REPL_WAL_ACK, 0, ack);
    }
}

void WalConsumer::loop_() {
    load_state_();

    while (running_) {
        auto client_r = QuicClient::create();
        if (!client_r.is_ok()) {
            spdlog::warn("wal_consumer: QuicClient::create failed: {}", client_r.error().message);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        auto& client = *client_r.value();

        {
            std::lock_guard<std::mutex> lk(resp_mu_);
            while (!resp_queue_.empty())
                resp_queue_.pop();
        }

        client.set_frame_handler([this](server::FrameType t, u32 qid, const byte* d, usize l) {
            on_frame_(t, qid, d, l);
        });

        auto auth_r = connect_and_auth_(client);
        if (auth_r.is_err()) {
            spdlog::warn("wal_consumer: connect/auth failed: {}", auth_r.error().message);
            client.close();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        spdlog::info("wal_consumer: connected to leader, confirmed_lsn={}", confirmed_lsn_.load());
        poll_loop_(client);
        client.close();

        if (running_)
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

} // namespace nyx::replication
