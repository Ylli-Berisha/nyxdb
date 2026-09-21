#include "database/database.h"
#include "server/server.h"
#include "server/wire.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <msquic.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace nyx;
using namespace nyx::server;

static const std::string TEST_ROOT = "/tmp/nyxdb_server_test";
static constexpr u16 TEST_PORT = 14433;
static const std::string TEST_TOKEN = "test-secret";

struct ClientCtx {
    std::atomic<bool> connected{false};
    std::atomic<bool> stream_ready{false};
    std::vector<byte> recv_buf;
    std::mutex mu;
    std::condition_variable cv;
    HQUIC stream = nullptr;
    const QUIC_API_TABLE* api = nullptr;
};

static QUIC_STATUS QUIC_API client_conn_cb(HQUIC, void* ctx, QUIC_CONNECTION_EVENT* ev) {
    auto* c = static_cast<ClientCtx*>(ctx);
    if (ev->Type == QUIC_CONNECTION_EVENT_CONNECTED) {
        c->connected.store(true);
        c->cv.notify_all();
    }
    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API client_stream_cb(HQUIC, void* ctx, QUIC_STREAM_EVENT* ev) {
    auto* c = static_cast<ClientCtx*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        c->stream_ready.store(true);
        c->cv.notify_all();
        break;
    case QUIC_STREAM_EVENT_RECEIVE: {
        std::lock_guard<std::mutex> lk(c->mu);
        for (u32 i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            const byte* b = reinterpret_cast<const byte*>(ev->RECEIVE.Buffers[i].Buffer);
            c->recv_buf.insert(c->recv_buf.end(), b, b + ev->RECEIVE.Buffers[i].Length);
        }
        c->cv.notify_all();
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

class ServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(TEST_ROOT);
        server_ = std::make_unique<Server>([] {
            auto r = Server::create(TEST_ROOT, TEST_PORT, TEST_TOKEN);
            EXPECT_TRUE(r.is_ok()) << r.error().message;
            return std::move(r.value());
        }());

        auto sr = server_->start();
        ASSERT_TRUE(sr.is_ok()) << sr.error().message;

        ASSERT_QUIC_OK(MsQuicOpen2(&api_));

        static const QUIC_REGISTRATION_CONFIG rc = {"nyxdb_test",
                                                    QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        ASSERT_QUIC_OK(api_->RegistrationOpen(&rc, &reg_));

        QUIC_SETTINGS s{};
        s.IdleTimeoutMs = 60000;
        s.IsSet.IdleTimeoutMs = 1;
        static const QUIC_BUFFER alpn = {5, (uint8_t*)"nyxdb"};
        ASSERT_QUIC_OK(api_->ConfigurationOpen(reg_, &alpn, 1, &s, sizeof(s), nullptr, &cfg_));

        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        ASSERT_QUIC_OK(api_->ConfigurationLoadCredential(cfg_, &cred));
    }

    void TearDown() override {
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
        server_.reset();
        fs::remove_all(TEST_ROOT);
    }

    void connect() {
        cctx_.api = api_;
        ASSERT_QUIC_OK(api_->ConnectionOpen(reg_, client_conn_cb, &cctx_, &conn_));
        ASSERT_QUIC_OK(
            api_->ConnectionStart(conn_, cfg_, QUIC_ADDRESS_FAMILY_INET, "127.0.0.1", TEST_PORT));

        {
            std::unique_lock<std::mutex> lk(cctx_.mu);
            cctx_.cv.wait_for(lk, std::chrono::seconds(5), [&] { return cctx_.connected.load(); });
            ASSERT_TRUE(cctx_.connected.load());
        }

        auto so =
            api_->StreamOpen(conn_, QUIC_STREAM_OPEN_FLAG_NONE, client_stream_cb, &cctx_, &stream_);
        ASSERT_QUIC_OK(so);
        api_->StreamStart(stream_, QUIC_STREAM_START_FLAG_NONE);

        {
            std::unique_lock<std::mutex> lk2(cctx_.mu);
            cctx_.cv.wait_for(lk2, std::chrono::seconds(5),
                              [&] { return cctx_.stream_ready.load(); });
            ASSERT_TRUE(cctx_.stream_ready.load());
            cctx_.stream = stream_;
        }
    }

    void send_frame(std::vector<byte> buf) {
        usize n = buf.size();
        byte* raw = new byte[n];
        std::memcpy(raw, buf.data(), n);
        QUIC_BUFFER* qbuf = new QUIC_BUFFER;
        qbuf->Buffer = raw;
        qbuf->Length = static_cast<u32>(n);
        api_->StreamSend(stream_, qbuf, 1, QUIC_SEND_FLAG_NONE, qbuf);
    }

    std::vector<byte> recv_frame(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(cctx_.mu);
        bool ok = cctx_.cv.wait_for(lk, timeout,
                                    [&] { return cctx_.recv_buf.size() >= FRAME_HEADER_SIZE; });
        if (!ok)
            return {};

        FrameHeader hdr;
        if (!decode_header(cctx_.recv_buf.data(), cctx_.recv_buf.size(), hdr))
            return {};

        ok = cctx_.cv.wait_for(lk, timeout, [&] { return cctx_.recv_buf.size() >= hdr.length; });
        if (!ok)
            return {};

        std::vector<byte> frame(cctx_.recv_buf.begin(), cctx_.recv_buf.begin() + hdr.length);
        cctx_.recv_buf.erase(cctx_.recv_buf.begin(), cctx_.recv_buf.begin() + hdr.length);
        return frame;
    }

    FrameHeader parse_header(const std::vector<byte>& frame) {
        FrameHeader h;
        decode_header(frame.data(), frame.size(), h);
        return h;
    }

    void send_auth(u32 qid, const std::string& token) {
        std::vector<byte> buf;
        encode_header(buf, FrameType::AUTH_REQ, qid, static_cast<u32>(2 + token.size()));
        encode_str(buf, token);
        send_frame(std::move(buf));
    }

    void send_query(u32 qid, const std::string& sql) {
        std::vector<byte> buf;
        encode_header(buf, FrameType::QUERY, qid, static_cast<u32>(2 + sql.size()));
        encode_str(buf, sql);
        send_frame(std::move(buf));
    }

    void setup_table() {
        send_query(1, "CREATE TABLE t (id INT NOT NULL, val VARCHAR(64))");
        auto meta = recv_frame();
        ASSERT_EQ(parse_header(meta).type, FrameType::RESULT_META);
        auto end = recv_frame();
        ASSERT_EQ(parse_header(end).type, FrameType::RESULT_END);

        send_query(2, "INSERT INTO t VALUES (1, 'hello'), (2, 'world')");
        meta = recv_frame();
        ASSERT_EQ(parse_header(meta).type, FrameType::RESULT_META);
        end = recv_frame();
        ASSERT_EQ(parse_header(end).type, FrameType::RESULT_END);
    }

    static void ASSERT_QUIC_OK(QUIC_STATUS s) {
        ASSERT_TRUE(QUIC_SUCCEEDED(s)) << "QUIC status: " << s;
    }

    std::unique_ptr<Server> server_;
    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC reg_ = nullptr;
    HQUIC cfg_ = nullptr;
    HQUIC conn_ = nullptr;
    HQUIC stream_ = nullptr;
    ClientCtx cctx_;
};

TEST_F(ServerTest, AuthOkWithCorrectToken) {
    connect();
    send_auth(0, TEST_TOKEN);
    auto frame = recv_frame();
    ASSERT_EQ(parse_header(frame).type, FrameType::AUTH_OK);
}

TEST_F(ServerTest, AuthErrWithWrongToken) {
    connect();
    send_auth(0, "wrong-token");
    auto frame = recv_frame();
    ASSERT_EQ(parse_header(frame).type, FrameType::AUTH_ERR);
}

TEST_F(ServerTest, QueryErrOnBadSql) {
    connect();
    send_auth(0, TEST_TOKEN);
    recv_frame();

    send_query(1, "SELECT * FROM nonexistent_table_xyz");
    auto frame = recv_frame();
    ASSERT_EQ(parse_header(frame).type, FrameType::QUERY_ERR);
}

TEST_F(ServerTest, SimpleSelectAfterInsert) {
    connect();
    send_auth(0, TEST_TOKEN);
    recv_frame();

    setup_table();

    send_query(3, "SELECT id FROM t ORDER BY id");
    auto meta = recv_frame();
    ASSERT_EQ(parse_header(meta).type, FrameType::RESULT_META);
    auto col = recv_frame();
    ASSERT_EQ(parse_header(col).type, FrameType::RESULT_COL);
    auto end = recv_frame();
    ASSERT_EQ(parse_header(end).type, FrameType::RESULT_END);
}

TEST_F(ServerTest, ResultMetaMatchesSchema) {
    connect();
    send_auth(0, TEST_TOKEN);
    recv_frame();

    send_query(1, "CREATE TABLE s (x INT NOT NULL, y VARCHAR(32))");
    recv_frame();
    recv_frame();

    send_query(2, "SELECT x, y FROM s");
    auto meta = recv_frame();
    ASSERT_EQ(parse_header(meta).type, FrameType::RESULT_META);

    const byte* p = meta.data() + FRAME_HEADER_SIZE;
    u16 col_count = static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
    ASSERT_EQ(col_count, 2u);
    recv_frame();
}

TEST_F(ServerTest, ResultColValuesCorrect) {
    connect();
    send_auth(0, TEST_TOKEN);
    recv_frame();

    send_query(1, "CREATE TABLE v (n INT NOT NULL)");
    recv_frame();
    recv_frame();
    send_query(2, "INSERT INTO v VALUES (42)");
    recv_frame();
    recv_frame();

    send_query(3, "SELECT n FROM v");
    recv_frame();
    auto col = recv_frame();
    ASSERT_EQ(parse_header(col).type, FrameType::RESULT_COL);

    const byte* p = col.data() + FRAME_HEADER_SIZE;
    p += 2; // col_idx
    u64 row_count = 0;
    for (int i = 0; i < 8; ++i)
        row_count |= static_cast<u64>(p[i]) << (8 * i);
    ASSERT_EQ(row_count, 1u);

    p += 8;
    ASSERT_EQ(p[0], 0u); // is_null = false
    i32 val = static_cast<i32>(p[1]) | (static_cast<i32>(p[2]) << 8) |
              (static_cast<i32>(p[3]) << 16) | (static_cast<i32>(p[4]) << 24);
    ASSERT_EQ(val, 42);

    recv_frame();
}

TEST_F(ServerTest, MultipleQueriesSameSession) {
    connect();
    send_auth(0, TEST_TOKEN);
    recv_frame();

    send_query(1, "CREATE TABLE m (k INT NOT NULL)");
    recv_frame();
    recv_frame();

    for (u32 qid = 2; qid <= 5; ++qid) {
        send_query(qid, "SELECT k FROM m");
        auto meta = recv_frame();
        EXPECT_EQ(parse_header(meta).type, FrameType::RESULT_META);
        auto col = recv_frame();
        EXPECT_EQ(parse_header(col).type, FrameType::RESULT_COL);
        auto end = recv_frame();
        EXPECT_EQ(parse_header(end).type, FrameType::RESULT_END);
    }
}
