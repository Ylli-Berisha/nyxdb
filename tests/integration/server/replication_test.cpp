#include "replication/replication_config.h"
#include "server/server.h"
#include "server/wire.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <msquic.h>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace nyx;
using namespace nyx::server;
using namespace nyx::replication;

static constexpr u16 LEADER_PORT = 14440;
static constexpr u16 FOLLOWER_PORT = 14441;
static const std::string TOKEN = "repl-test-secret";
static const std::string LEADER_ROOT = "/tmp/nyxdb_repl_test_leader";
static const std::string FOLLOWER_ROOT = "/tmp/nyxdb_repl_test_follower";

// ---------------------------------------------------------------------------
// QUIC callbacks for test clients
// ---------------------------------------------------------------------------

struct TcCtx {
    std::atomic<bool> connected{false};
    std::atomic<bool> stream_ready{false};
    std::vector<byte> recv_buf;
    std::mutex mu;
    std::condition_variable cv;
};

static QUIC_STATUS QUIC_API tc_conn_cb(HQUIC, void* c, QUIC_CONNECTION_EVENT* ev) {
    auto* ctx = static_cast<TcCtx*>(c);
    if (ev->Type == QUIC_CONNECTION_EVENT_CONNECTED) {
        ctx->connected.store(true);
        ctx->cv.notify_all();
    }
    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API tc_stream_cb(HQUIC, void* c, QUIC_STREAM_EVENT* ev) {
    auto* ctx = static_cast<TcCtx*>(c);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        ctx->stream_ready.store(true);
        ctx->cv.notify_all();
        break;
    case QUIC_STREAM_EVENT_RECEIVE: {
        std::lock_guard<std::mutex> lk(ctx->mu);
        for (u32 i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            const byte* b = reinterpret_cast<const byte*>(ev->RECEIVE.Buffers[i].Buffer);
            ctx->recv_buf.insert(ctx->recv_buf.end(), b, b + ev->RECEIVE.Buffers[i].Length);
        }
        ctx->cv.notify_all();
    } break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* qbuf = static_cast<QUIC_BUFFER*>(ev->SEND_COMPLETE.ClientContext);
        delete[] qbuf->Buffer;
        delete qbuf;
    } break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// QueryClient: thin QUIC wrapper for test queries
// ---------------------------------------------------------------------------

class QueryClient {
  public:
    QueryClient() = default;
    ~QueryClient() { close(); }
    QueryClient(const QueryClient&) = delete;
    QueryClient& operator=(const QueryClient&) = delete;

    bool connect(u16 port) {
        if (QUIC_FAILED(MsQuicOpen2(&api_)))
            return false;
        static const QUIC_REGISTRATION_CONFIG rc{"nyxdb_tc", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api_->RegistrationOpen(&rc, &reg_)))
            return false;

        QUIC_SETTINGS s{};
        s.IdleTimeoutMs = 60000;
        s.IsSet.IdleTimeoutMs = 1;
        static const QUIC_BUFFER alpn{5, (uint8_t*)"nyxdb"};
        if (QUIC_FAILED(api_->ConfigurationOpen(reg_, &alpn, 1, &s, sizeof(s), nullptr, &cfg_)))
            return false;

        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        if (QUIC_FAILED(api_->ConfigurationLoadCredential(cfg_, &cred)))
            return false;

        if (QUIC_FAILED(api_->ConnectionOpen(reg_, tc_conn_cb, &ctx_, &conn_)))
            return false;
        if (QUIC_FAILED(
                api_->ConnectionStart(conn_, cfg_, QUIC_ADDRESS_FAMILY_INET, "127.0.0.1", port)))
            return false;

        {
            std::unique_lock<std::mutex> lk(ctx_.mu);
            if (!ctx_.cv.wait_for(lk, std::chrono::seconds(5),
                                  [this] { return ctx_.connected.load(); }))
                return false;
        }

        if (QUIC_FAILED(
                api_->StreamOpen(conn_, QUIC_STREAM_OPEN_FLAG_NONE, tc_stream_cb, &ctx_, &stream_)))
            return false;
        api_->StreamStart(stream_, QUIC_STREAM_START_FLAG_NONE);

        {
            std::unique_lock<std::mutex> lk(ctx_.mu);
            if (!ctx_.cv.wait_for(lk, std::chrono::seconds(5),
                                  [this] { return ctx_.stream_ready.load(); }))
                return false;
        }
        return true;
    }

    bool auth(const std::string& token) {
        std::vector<byte> buf;
        encode_header(buf, FrameType::AUTH_REQ, next_qid_++, static_cast<u32>(2 + token.size()));
        encode_str(buf, token);
        send_raw(std::move(buf));
        auto frame = recv_frame();
        if (frame.empty())
            return false;
        FrameHeader h;
        decode_header(frame.data(), frame.size(), h);
        return h.type == FrameType::AUTH_OK;
    }

    // Execute a write or DDL statement. Returns rows_affected, or -1 on error.
    i64 exec(const std::string& sql) {
        u32 qid = next_qid_++;
        std::vector<byte> buf;
        encode_header(buf, FrameType::QUERY, qid, static_cast<u32>(2 + sql.size()));
        encode_str(buf, sql);
        send_raw(std::move(buf));

        while (true) {
            auto frame = recv_frame();
            if (frame.empty())
                return -1;
            FrameHeader h;
            decode_header(frame.data(), frame.size(), h);
            if (h.type == FrameType::QUERY_ERR)
                return -1;
            if (h.type == FrameType::RESULT_END) {
                if (frame.size() < FRAME_HEADER_SIZE + 8)
                    return 0;
                i64 rows = 0;
                for (int i = 0; i < 8; ++i)
                    rows |= static_cast<i64>(frame[FRAME_HEADER_SIZE + i]) << (8 * i);
                return rows;
            }
        }
    }

    // Execute a SELECT and return the row count from the first column.
    // Returns -1 on any error (table missing, timeout, etc.).
    i64 query_row_count(const std::string& sql) {
        u32 qid = next_qid_++;
        std::vector<byte> buf;
        encode_header(buf, FrameType::QUERY, qid, static_cast<u32>(2 + sql.size()));
        encode_str(buf, sql);
        send_raw(std::move(buf));

        // First frame — META or ERR
        {
            auto frame = recv_frame();
            if (frame.empty())
                return -1;
            FrameHeader h;
            decode_header(frame.data(), frame.size(), h);
            if (h.type != FrameType::RESULT_META)
                return -1;
        }

        i64 row_count = 0;
        bool got_col = false;
        while (true) {
            auto frame = recv_frame();
            if (frame.empty())
                return -1;
            FrameHeader h;
            decode_header(frame.data(), frame.size(), h);
            if (h.type == FrameType::RESULT_END)
                break;
            if (h.type == FrameType::RESULT_COL && !got_col) {
                // payload: u16(col_idx) u64(row_count) rows...
                if (frame.size() >= FRAME_HEADER_SIZE + 2 + 8) {
                    row_count = 0;
                    for (int i = 0; i < 8; ++i)
                        row_count |= static_cast<i64>(frame[FRAME_HEADER_SIZE + 2 + i]) << (8 * i);
                    got_col = true;
                }
            }
        }
        return row_count;
    }

    void send_raw(std::vector<byte> buf) {
        usize n = buf.size();
        byte* raw = new byte[n];
        std::memcpy(raw, buf.data(), n);
        QUIC_BUFFER* qbuf = new QUIC_BUFFER;
        qbuf->Buffer = raw;
        qbuf->Length = static_cast<u32>(n);
        api_->StreamSend(stream_, qbuf, 1, QUIC_SEND_FLAG_NONE, qbuf);
    }

    std::vector<byte> recv_frame(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(ctx_.mu);
        bool ok = ctx_.cv.wait_for(lk, timeout, [this] {
            if (ctx_.recv_buf.size() < FRAME_HEADER_SIZE)
                return false;
            FrameHeader h;
            if (!decode_header(ctx_.recv_buf.data(), ctx_.recv_buf.size(), h))
                return false;
            return ctx_.recv_buf.size() >= h.length;
        });
        if (!ok)
            return {};
        FrameHeader h;
        decode_header(ctx_.recv_buf.data(), ctx_.recv_buf.size(), h);
        std::vector<byte> frame(ctx_.recv_buf.begin(), ctx_.recv_buf.begin() + h.length);
        ctx_.recv_buf.erase(ctx_.recv_buf.begin(), ctx_.recv_buf.begin() + h.length);
        return frame;
    }

    void close() {
        if (stream_) {
            api_->StreamClose(stream_);
            stream_ = nullptr;
        }
        if (conn_) {
            api_->ConnectionClose(conn_);
            conn_ = nullptr;
        }
        if (cfg_) {
            api_->ConfigurationClose(cfg_);
            cfg_ = nullptr;
        }
        if (reg_) {
            api_->RegistrationClose(reg_);
            reg_ = nullptr;
        }
        if (api_) {
            MsQuicClose(api_);
            api_ = nullptr;
        }
    }

  private:
    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC reg_ = nullptr, cfg_ = nullptr, conn_ = nullptr, stream_ = nullptr;
    TcCtx ctx_;
    u32 next_qid_ = 1;
};

// ---------------------------------------------------------------------------
// Helper: build NodeConfig for tests with short election/heartbeat timings
// ---------------------------------------------------------------------------

static NodeConfig make_leader_cfg() {
    NodeConfig cfg;
    cfg.node_id = "127.0.0.1";
    cfg.role = NodeConfig::Role::Leader;
    cfg.port = LEADER_PORT;
    cfg.peer_addrs = {"127.0.0.1:14440", "127.0.0.2:14441"};
    cfg.auth_token = TOKEN;
    cfg.election_timeout_min_ms = 500;
    cfg.election_timeout_max_ms = 1000;
    cfg.heartbeat_interval_ms = 200;
    return cfg;
}

static NodeConfig make_follower_cfg() {
    NodeConfig cfg;
    cfg.node_id = "127.0.0.2";
    cfg.role = NodeConfig::Role::Follower;
    cfg.port = FOLLOWER_PORT;
    cfg.peer_addrs = {"127.0.0.1:14440", "127.0.0.2:14441"};
    cfg.auth_token = TOKEN;
    cfg.leader_addr = "127.0.0.1:14440";
    cfg.election_timeout_min_ms = 500;
    cfg.election_timeout_max_ms = 1000;
    cfg.heartbeat_interval_ms = 200;
    return cfg;
}

// ---------------------------------------------------------------------------
// ReplicationTest fixture
// ---------------------------------------------------------------------------

class ReplicationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(LEADER_ROOT);
        fs::remove_all(FOLLOWER_ROOT);

        {
            auto r = Server::create(LEADER_ROOT, LEADER_PORT, TOKEN, make_leader_cfg());
            ASSERT_TRUE(r.is_ok()) << r.error().message;
            leader_ = std::make_unique<Server>(std::move(r.value()));
            ASSERT_TRUE(leader_->start().is_ok());
        }
        {
            auto r = Server::create(FOLLOWER_ROOT, FOLLOWER_PORT, TOKEN, make_follower_cfg());
            ASSERT_TRUE(r.is_ok()) << r.error().message;
            follower_ = std::make_unique<Server>(std::move(r.value()));
            ASSERT_TRUE(follower_->start().is_ok());
        }
    }

    void TearDown() override {
        follower_.reset();
        leader_.reset();
        fs::remove_all(LEADER_ROOT);
        fs::remove_all(FOLLOWER_ROOT);
    }

    bool wait_for(std::function<bool()> fn, std::chrono::seconds timeout = std::chrono::seconds(8),
                  std::chrono::milliseconds poll = std::chrono::milliseconds(200)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (fn())
                return true;
            std::this_thread::sleep_for(poll);
        }
        return false;
    }

    // Open a fresh QueryClient to `port`, auth, SELECT, return row count.
    // Returns -2 on connection/auth failure, -1 on query error (table missing).
    i64 select_count(u16 port, const std::string& sql) {
        QueryClient c;
        if (!c.connect(port))
            return -2;
        if (!c.auth(TOKEN))
            return -2;
        return c.query_row_count(sql);
    }

    std::unique_ptr<Server> leader_;
    std::unique_ptr<Server> follower_;
};

// ---------------------------------------------------------------------------
// Test 1: follower syncs snapshot + WAL from leader
// ---------------------------------------------------------------------------

TEST_F(ReplicationTest, FollowerReplicatesFromLeader) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE rep_t (id INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO rep_t VALUES (1), (2), (3)"), 0);
    }

    bool ok = wait_for([this] { return select_count(FOLLOWER_PORT, "SELECT id FROM rep_t") == 3; });
    EXPECT_TRUE(ok) << "follower did not replicate 3 rows within timeout";
}

// ---------------------------------------------------------------------------
// Test 2: write to follower is forwarded to leader and replicated back
// ---------------------------------------------------------------------------

TEST_F(ReplicationTest, WriteForwardingFromFollower) {
    {
        QueryClient fc;
        ASSERT_TRUE(fc.connect(FOLLOWER_PORT));
        ASSERT_TRUE(fc.auth(TOKEN));
        ASSERT_GE(fc.exec("CREATE TABLE fwd_t (x INT NOT NULL)"), 0);
        ASSERT_GE(fc.exec("INSERT INTO fwd_t VALUES (10), (20)"), 0);
    }

    bool on_follower =
        wait_for([this] { return select_count(FOLLOWER_PORT, "SELECT x FROM fwd_t") == 2; });
    EXPECT_TRUE(on_follower) << "forwarded writes not visible on follower within timeout";

    EXPECT_EQ(select_count(LEADER_PORT, "SELECT x FROM fwd_t"), 2);
}

// ---------------------------------------------------------------------------
// Test 3: follower becomes leader after leader failure
// ---------------------------------------------------------------------------

TEST_F(ReplicationTest, ElectionOnLeaderFailure) {
    // Seed one row on leader and wait for follower to sync it.
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE elect_t (v INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO elect_t VALUES (1)"), 0);
    }

    bool synced =
        wait_for([this] { return select_count(FOLLOWER_PORT, "SELECT v FROM elect_t") == 1; });
    ASSERT_TRUE(synced) << "follower did not sync initial data before leader failure";

    // Kill the leader.
    leader_.reset();

    // Follower must detect the missing heartbeat and win election.
    // election_timeout_max_ms=1000 → detects within ~1s; add slack for processing.
    bool elected = wait_for([this] { return follower_->is_leader(); }, std::chrono::seconds(3),
                            std::chrono::milliseconds(50));
    ASSERT_TRUE(elected) << "follower did not become leader within 3s";

    // New leader should execute writes directly (no forwarding).
    QueryClient fc;
    ASSERT_TRUE(fc.connect(FOLLOWER_PORT));
    ASSERT_TRUE(fc.auth(TOKEN));
    ASSERT_EQ(fc.exec("INSERT INTO elect_t VALUES (2)"), 1);
    EXPECT_EQ(fc.query_row_count("SELECT v FROM elect_t"), 2);
}
