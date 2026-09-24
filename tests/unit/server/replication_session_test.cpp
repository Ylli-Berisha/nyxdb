#include "database/database.h"
#include "replication/replication_config.h"
#include "replication/replication_manager.h"
#include "server/replication_session.h"
#include "server/wire.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <msquic.h>

using namespace nyx;
using namespace nyx::server;
using namespace nyx::replication;
namespace fs = std::filesystem;

static const std::string RS_DB_ROOT = "/tmp/nyxdb_repl_session_test";

static char g_rs_fake_storage{};
static HQUIC RS_FAKE_H = reinterpret_cast<HQUIC>(&g_rs_fake_storage);

static std::vector<std::vector<byte>> g_rs_sent;

static QUIC_STATUS rs_mock_stream_send(HQUIC, const QUIC_BUFFER* const bufs, uint32_t count,
                                       QUIC_SEND_FLAGS, void* ctx) {
    for (uint32_t i = 0; i < count; ++i)
        g_rs_sent.push_back({reinterpret_cast<const byte*>(bufs[i].Buffer),
                             reinterpret_cast<const byte*>(bufs[i].Buffer) + bufs[i].Length});
    auto* qb = static_cast<QUIC_BUFFER*>(ctx);
    if (qb) {
        delete[] qb->Buffer;
        delete qb;
    }
    return 0;
}

static void rs_mock_conn_shutdown(HQUIC, QUIC_CONNECTION_SHUTDOWN_FLAGS, QUIC_UINT62) {}

static std::vector<byte> make_repl_hello(const std::string& node_id, u64 lsn) {
    std::vector<byte> pay;
    encode_u16(pay, static_cast<u16>(node_id.size()));
    for (char c : node_id)
        pay.push_back(static_cast<byte>(c));
    encode_u64(pay, lsn);
    std::vector<byte> buf;
    encode_header(buf, FrameType::REPL_HELLO, 0, static_cast<u32>(pay.size()));
    buf.insert(buf.end(), pay.begin(), pay.end());
    return buf;
}

static std::vector<byte> make_repl_frame(FrameType ft, std::vector<byte> payload = {}) {
    std::vector<byte> buf;
    encode_header(buf, ft, 0, static_cast<u32>(payload.size()));
    buf.insert(buf.end(), payload.begin(), payload.end());
    return buf;
}

class ReplSessionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        api_ = {};
        api_.StreamSend = rs_mock_stream_send;
        api_.ConnectionShutdown = rs_mock_conn_shutdown;
        g_rs_sent.clear();
    }
    void TearDown() override {}

    ReplicationSession make_session(ReplicationManager* repl = nullptr) {
        return ReplicationSession(repl, RS_FAKE_H, &api_);
    }

    QUIC_API_TABLE api_{};
};

TEST_F(ReplSessionTest, NullReplReplHello) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    auto f = make_repl_hello("node1", 100);
    EXPECT_NO_FATAL_FAILURE(s.on_data(f.data(), f.size()));
    EXPECT_TRUE(g_rs_sent.empty());
}

TEST_F(ReplSessionTest, NullReplHeartbeat) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    std::vector<byte> pay(8, byte{0});
    auto f = make_repl_frame(FrameType::REPL_HEARTBEAT, pay);
    EXPECT_NO_FATAL_FAILURE(s.on_data(f.data(), f.size()));
}

TEST_F(ReplSessionTest, NullReplVoteReq) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    std::vector<byte> pay(16, byte{0});
    auto f = make_repl_frame(FrameType::REPL_VOTE_REQ, pay);
    EXPECT_NO_FATAL_FAILURE(s.on_data(f.data(), f.size()));
}

TEST_F(ReplSessionTest, NullReplVoteResp) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    std::vector<byte> pay(9, byte{0});
    auto f = make_repl_frame(FrameType::REPL_VOTE_RESP, pay);
    EXPECT_NO_FATAL_FAILURE(s.on_data(f.data(), f.size()));
}

TEST_F(ReplSessionTest, NullReplWalPull) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    auto f = make_repl_frame(FrameType::REPL_WAL_PULL);
    EXPECT_NO_FATAL_FAILURE(s.on_data(f.data(), f.size()));
}

TEST_F(ReplSessionTest, ReplHelloTruncatedBelowTwoBytes) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    std::vector<byte> buf;
    encode_header(buf, FrameType::REPL_HELLO, 0, 1);
    buf.push_back(byte{0x05});
    EXPECT_NO_FATAL_FAILURE(s.on_data(buf.data(), buf.size()));
}

TEST_F(ReplSessionTest, ReplHelloTruncatedNodeId) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    std::vector<byte> pay;
    encode_u16(pay, 10);
    pay.insert(pay.end(), 4, byte{'x'});
    std::vector<byte> buf;
    encode_header(buf, FrameType::REPL_HELLO, 0, static_cast<u32>(pay.size()));
    buf.insert(buf.end(), pay.begin(), pay.end());
    EXPECT_NO_FATAL_FAILURE(s.on_data(buf.data(), buf.size()));
}

TEST_F(ReplSessionTest, PartialFrameNoAction) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    auto f = make_repl_hello("n1", 0);
    s.on_data(f.data(), f.size() / 2);
    EXPECT_TRUE(g_rs_sent.empty());
}

TEST_F(ReplSessionTest, SplitFrameAcrossTwoCalls) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    auto f = make_repl_hello("n1", 42);
    usize mid = f.size() / 2;
    s.on_data(f.data(), mid);
    EXPECT_NO_FATAL_FAILURE(s.on_data(f.data() + mid, f.size() - mid));
}

TEST_F(ReplSessionTest, TwoHelloFramesInOneCall) {
    auto s = make_session();
    s.on_stream(RS_FAKE_H);
    auto f1 = make_repl_hello("node-a", 10);
    auto f2 = make_repl_hello("node-b", 20);
    std::vector<byte> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());
    EXPECT_NO_FATAL_FAILURE(s.on_data(combined.data(), combined.size()));
}

TEST_F(ReplSessionTest, StandaloneReplHelloNocrash) {
    fs::remove_all(RS_DB_ROOT);
    auto dr = Database::open(RS_DB_ROOT);
    ASSERT_TRUE(dr.is_ok()) << dr.error().message;
    Database db(std::move(dr.value()));

    NodeConfig cfg;
    cfg.node_id = "coord";
    cfg.role = NodeConfig::Role::Standalone;
    auto repl = ReplicationManager::create(cfg, &db, RS_DB_ROOT);
    repl->start();

    {
        auto s = make_session(repl.get());
        s.on_stream(RS_FAKE_H);
        auto f = make_repl_hello("follower-1", 500);
        EXPECT_NO_FATAL_FAILURE(s.on_data(f.data(), f.size()));
        EXPECT_TRUE(g_rs_sent.empty());
    }

    repl->stop();
    fs::remove_all(RS_DB_ROOT);
}
