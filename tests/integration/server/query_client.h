#pragma once

#include "server/wire.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <msquic.h>
#include <mutex>
#include <vector>

namespace nyx::server {

struct QcCtx {
    std::atomic<bool> connected{false};
    std::atomic<bool> stream_ready{false};
    std::vector<byte> recv_buf;
    std::mutex mu;
    std::condition_variable cv;
};

static QUIC_STATUS QUIC_API qc_conn_cb(HQUIC, void* c, QUIC_CONNECTION_EVENT* ev) {
    auto* ctx = static_cast<QcCtx*>(c);
    if (ev->Type == QUIC_CONNECTION_EVENT_CONNECTED) {
        ctx->connected.store(true);
        ctx->cv.notify_all();
    }
    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API qc_stream_cb(HQUIC, void* c, QUIC_STREAM_EVENT* ev) {
    auto* ctx = static_cast<QcCtx*>(c);
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
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* qbuf = static_cast<QUIC_BUFFER*>(ev->SEND_COMPLETE.ClientContext);
        delete[] qbuf->Buffer;
        delete qbuf;
        break;
    }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

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

        if (QUIC_FAILED(api_->ConnectionOpen(reg_, qc_conn_cb, &ctx_, &conn_)))
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
                api_->StreamOpen(conn_, QUIC_STREAM_OPEN_FLAG_NONE, qc_stream_cb, &ctx_, &stream_)))
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

    i64 query_row_count(const std::string& sql) {
        u32 qid = next_qid_++;
        std::vector<byte> buf;
        encode_header(buf, FrameType::QUERY, qid, static_cast<u32>(2 + sql.size()));
        encode_str(buf, sql);
        send_raw(std::move(buf));
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
    QcCtx ctx_;
    u32 next_qid_ = 1;
};

} // namespace nyx::server
