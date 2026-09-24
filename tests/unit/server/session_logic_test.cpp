#include "database/database.h"
#include "server/session.h"
#include "server/wire.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <msquic.h>

using namespace nyx;
using namespace nyx::server;
namespace fs = std::filesystem;

static const std::string DB_ROOT = "/tmp/nyxdb_session_logic_test";
static const std::string TOKEN = "unit-test-token";

static char g_fake_storage{};
static HQUIC FAKE_H = reinterpret_cast<HQUIC>(&g_fake_storage);

static std::vector<std::vector<byte>> g_sent;
static bool g_shutdown = false;

static QUIC_STATUS mock_stream_send(HQUIC, const QUIC_BUFFER* const bufs, uint32_t count,
                                    QUIC_SEND_FLAGS, void* ctx) {
    for (uint32_t i = 0; i < count; ++i)
        g_sent.push_back({reinterpret_cast<const byte*>(bufs[i].Buffer),
                          reinterpret_cast<const byte*>(bufs[i].Buffer) + bufs[i].Length});
    auto* qb = static_cast<QUIC_BUFFER*>(ctx);
    if (qb) {
        delete[] qb->Buffer;
        delete qb;
    }
    return 0;
}

static void mock_conn_shutdown(HQUIC, QUIC_CONNECTION_SHUTDOWN_FLAGS, QUIC_UINT62) {
    g_shutdown = true;
}

static std::vector<byte> make_auth_req(u32 qid, const std::string& tok) {
    std::vector<byte> pay;
    encode_str(pay, tok);
    std::vector<byte> buf;
    encode_header(buf, FrameType::AUTH_REQ, qid, static_cast<u32>(pay.size()));
    buf.insert(buf.end(), pay.begin(), pay.end());
    return buf;
}

static std::vector<byte> make_query(u32 qid, const std::string& sql) {
    std::vector<byte> pay;
    encode_str(pay, sql);
    std::vector<byte> buf;
    encode_header(buf, FrameType::QUERY, qid, static_cast<u32>(pay.size()));
    buf.insert(buf.end(), pay.begin(), pay.end());
    return buf;
}

static FrameType frame_type(const std::vector<byte>& f) {
    FrameHeader h;
    decode_header(f.data(), f.size(), h);
    return h.type;
}

class SessionLogicTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        fs::remove_all(DB_ROOT);
        auto r = Database::open(DB_ROOT);
        ASSERT_TRUE(r.is_ok()) << r.error().message;
        db_ = std::make_unique<Database>(std::move(r.value()));
    }
    static void TearDownTestSuite() {
        db_.reset();
        fs::remove_all(DB_ROOT);
    }

    void SetUp() override {
        api_ = {};
        api_.StreamSend = mock_stream_send;
        api_.ConnectionShutdown = mock_conn_shutdown;
        g_sent.clear();
        g_shutdown = false;
    }

    Session make_session(bool pre_authed = false) {
        return Session(db_.get(), TOKEN, FAKE_H, &api_, nullptr, pre_authed);
    }

    static std::unique_ptr<Database> db_;
    QUIC_API_TABLE api_{};
};

std::unique_ptr<Database> SessionLogicTest::db_;

TEST_F(SessionLogicTest, AuthOkCorrectToken) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    auto f = make_auth_req(1, TOKEN);
    s.on_data(f.data(), f.size());
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::AUTH_OK);
}

TEST_F(SessionLogicTest, AuthErrWrongToken) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    auto f = make_auth_req(1, "wrong-token");
    s.on_data(f.data(), f.size());
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::AUTH_ERR);
}

TEST_F(SessionLogicTest, AuthAlreadyAuthedIgnored) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    auto f = make_auth_req(1, TOKEN);
    s.on_data(f.data(), f.size());
    s.on_data(f.data(), f.size());
    EXPECT_EQ(g_sent.size(), 1u);
}

TEST_F(SessionLogicTest, AuthTruncatedPayloadIgnored) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    std::vector<byte> buf;
    encode_header(buf, FrameType::AUTH_REQ, 1, 1);
    buf.push_back(byte{0x00});
    s.on_data(buf.data(), buf.size());
    EXPECT_TRUE(g_sent.empty());
}

TEST_F(SessionLogicTest, QueryBeforeAuthGetsAuthErrAndShutdown) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    auto q = make_query(2, "SELECT 1");
    s.on_data(q.data(), q.size());
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::AUTH_ERR);
    EXPECT_TRUE(g_shutdown);
}

TEST_F(SessionLogicTest, QueryAfterAuthInvalidSqlGetsQueryErr) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    auto auth = make_auth_req(1, TOKEN);
    s.on_data(auth.data(), auth.size());
    g_sent.clear();

    auto q = make_query(2, "not valid sql!!!");
    s.on_data(q.data(), q.size());
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::QUERY_ERR);
}

TEST_F(SessionLogicTest, QueryAfterAuthValidSqlGetsResultFrames) {
    ASSERT_TRUE(db_->execute("CREATE TABLE t (id INT NOT NULL)").is_ok());
    ASSERT_TRUE(db_->execute("INSERT INTO t VALUES (42)").is_ok());

    auto s = make_session();
    s.on_stream(FAKE_H);
    auto auth = make_auth_req(1, TOKEN);
    s.on_data(auth.data(), auth.size());
    g_sent.clear();

    auto q = make_query(2, "SELECT id FROM t");
    s.on_data(q.data(), q.size());

    ASSERT_GE(g_sent.size(), 3u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::RESULT_META);
    EXPECT_EQ(frame_type(g_sent[1]), FrameType::RESULT_COL);
    EXPECT_EQ(frame_type(g_sent.back()), FrameType::RESULT_END);
}

TEST_F(SessionLogicTest, PreAuthedSessionRunsQueryDirectly) {
    auto s = make_session(true);
    s.on_stream(FAKE_H);
    auto q = make_query(1, "not valid sql!!!");
    s.on_data(q.data(), q.size());
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::QUERY_ERR);
}

TEST_F(SessionLogicTest, PartialFrameNoResponse) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    auto f = make_auth_req(1, TOKEN);
    s.on_data(f.data(), 4);
    EXPECT_TRUE(g_sent.empty());
}

TEST_F(SessionLogicTest, SplitFrameAcrossTwoCalls) {
    auto s = make_session();
    s.on_stream(FAKE_H);
    auto f = make_auth_req(1, TOKEN);
    usize mid = f.size() / 2;
    s.on_data(f.data(), mid);
    EXPECT_TRUE(g_sent.empty());
    s.on_data(f.data() + mid, f.size() - mid);
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::AUTH_OK);
}

TEST_F(SessionLogicTest, TwoFramesInOneCall) {
    auto s = make_session();
    s.on_stream(FAKE_H);

    auto auth = make_auth_req(1, TOKEN);
    auto q = make_query(2, "bad sql!!!");
    std::vector<byte> combined;
    combined.insert(combined.end(), auth.begin(), auth.end());
    combined.insert(combined.end(), q.begin(), q.end());

    s.on_data(combined.data(), combined.size());
    ASSERT_EQ(g_sent.size(), 2u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::AUTH_OK);
    EXPECT_EQ(frame_type(g_sent[1]), FrameType::QUERY_ERR);
}

TEST_F(SessionLogicTest, NoStreamSendsAreDropped) {
    auto s = make_session();
    auto f = make_auth_req(1, TOKEN);
    s.on_data(f.data(), f.size());
    EXPECT_TRUE(g_sent.empty());
}

TEST_F(SessionLogicTest, ResultMetaColCount) {
    ASSERT_TRUE(db_->execute("CREATE TABLE cols (a INT NOT NULL, b DOUBLE NOT NULL)").is_ok());
    ASSERT_TRUE(db_->execute("INSERT INTO cols VALUES (1, 2.0)").is_ok());

    auto s = make_session();
    s.on_stream(FAKE_H);
    auto auth = make_auth_req(1, TOKEN);
    s.on_data(auth.data(), auth.size());
    g_sent.clear();

    auto q = make_query(2, "SELECT a, b FROM cols");
    s.on_data(q.data(), q.size());

    ASSERT_GE(g_sent.size(), 1u);
    EXPECT_EQ(frame_type(g_sent[0]), FrameType::RESULT_META);
    const auto& meta = g_sent[0];
    ASSERT_GE(meta.size(), FRAME_HEADER_SIZE + 2u);
    u16 col_count = decode_u16(meta.data() + FRAME_HEADER_SIZE);
    EXPECT_EQ(col_count, 2u);
}
