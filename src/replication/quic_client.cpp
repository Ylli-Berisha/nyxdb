#include "replication/quic_client.h"

#include "server/wire.h"

#include <chrono>
#include <cstring>

namespace nyx::replication {

static const QUIC_REGISTRATION_CONFIG k_reg_cfg = {"nyxdb-client",
                                                   QUIC_EXECUTION_PROFILE_LOW_LATENCY};
static const QUIC_BUFFER k_alpn = {5, (uint8_t*)"nyxdb"};

Result<std::unique_ptr<QuicClient>> QuicClient::create() {
    auto c = std::unique_ptr<QuicClient>(new QuicClient());

    if (QUIC_FAILED(MsQuicOpen2(&c->api_)))
        return Result<std::unique_ptr<QuicClient>>::err("MsQuicOpen2 failed");

    if (QUIC_FAILED(c->api_->RegistrationOpen(&k_reg_cfg, &c->registration_)))
        return Result<std::unique_ptr<QuicClient>>::err("RegistrationOpen failed");

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 30000;
    settings.IsSet.IdleTimeoutMs = 1;

    if (QUIC_FAILED(c->api_->ConfigurationOpen(c->registration_, &k_alpn, 1, &settings,
                                               sizeof(settings), nullptr, &c->configuration_)))
        return Result<std::unique_ptr<QuicClient>>::err("ConfigurationOpen failed");

    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    if (QUIC_FAILED(c->api_->ConfigurationLoadCredential(c->configuration_, &cred)))
        return Result<std::unique_ptr<QuicClient>>::err("ConfigurationLoadCredential failed");

    return Result<std::unique_ptr<QuicClient>>::ok(std::move(c));
}

QuicClient::~QuicClient() {
    close();
    if (configuration_)
        api_->ConfigurationClose(configuration_);
    if (registration_)
        api_->RegistrationClose(registration_);
    if (api_)
        MsQuicClose(api_);
}

Result<void> QuicClient::connect(const std::string& host, u16 port) {
    if (QUIC_FAILED(api_->ConnectionOpen(registration_, connection_cb_, this, &connection_)))
        return Result<void>::err("ConnectionOpen failed");

    if (QUIC_FAILED(api_->ConnectionStart(connection_, configuration_, QUIC_ADDRESS_FAMILY_UNSPEC,
                                          host.c_str(), port))) {
        api_->ConnectionClose(connection_);
        connection_ = nullptr;
        return Result<void>::err("ConnectionStart failed");
    }

    std::unique_lock<std::mutex> lk(connect_mu_);
    bool ok = connect_cv_.wait_for(lk, std::chrono::seconds(10),
                                   [this] { return stream_ready_ || connect_failed_; });
    if (!ok || connect_failed_)
        return Result<void>::err("connection failed or timed out");

    return Result<void>::ok();
}

Result<void> QuicClient::send_frame(server::FrameType type, u32 query_id,
                                    const std::vector<byte>& payload) {
    if (!stream_)
        return Result<void>::err("not connected");

    std::vector<byte> buf;
    server::encode_header(buf, type, query_id, static_cast<u32>(payload.size()));
    buf.insert(buf.end(), payload.begin(), payload.end());

    auto* qbuf = new QUIC_BUFFER;
    qbuf->Length = static_cast<uint32_t>(buf.size());
    qbuf->Buffer = new uint8_t[buf.size()];
    std::memcpy(qbuf->Buffer, buf.data(), buf.size());

    if (QUIC_FAILED(api_->StreamSend(stream_, qbuf, 1, QUIC_SEND_FLAG_NONE, qbuf))) {
        delete[] qbuf->Buffer;
        delete qbuf;
        return Result<void>::err("StreamSend failed");
    }

    return Result<void>::ok();
}

void QuicClient::set_frame_handler(
    std::function<void(server::FrameType, u32, const byte*, usize)> handler) {
    on_frame_ = std::move(handler);
}

void QuicClient::close() {
    if (stream_) {
        api_->StreamShutdown(stream_, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        stream_ = nullptr;
    }
    if (connection_) {
        api_->ConnectionShutdown(connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
}

QUIC_STATUS QUIC_API QuicClient::connection_cb_(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev) {
    auto* self = static_cast<QuicClient*>(ctx);
    switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        HQUIC stream = nullptr;
        if (QUIC_FAILED(self->api_->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE, stream_cb_, self,
                                               &stream)) ||
            QUIC_FAILED(self->api_->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
            if (stream)
                self->api_->StreamClose(stream);
            std::lock_guard<std::mutex> lk(self->connect_mu_);
            self->connect_failed_ = true;
            self->connect_cv_.notify_all();
        } else {
            self->stream_ = stream;
        }
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        self->api_->ConnectionClose(conn);
        self->connection_ = nullptr;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicClient::stream_cb_(HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev) {
    auto* self = static_cast<QuicClient*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE: {
        std::lock_guard<std::mutex> lk(self->connect_mu_);
        if (QUIC_SUCCEEDED(ev->START_COMPLETE.Status))
            self->stream_ready_ = true;
        else
            self->connect_failed_ = true;
        self->connect_cv_.notify_all();
        break;
    }
    case QUIC_STREAM_EVENT_RECEIVE:
        for (u32 i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            const byte* data = reinterpret_cast<const byte*>(ev->RECEIVE.Buffers[i].Buffer);
            usize len = ev->RECEIVE.Buffers[i].Length;
            self->recv_buf_.insert(self->recv_buf_.end(), data, data + len);
        }
        while (true) {
            server::FrameHeader hdr;
            if (!server::decode_header(self->recv_buf_.data(), self->recv_buf_.size(), hdr))
                break;
            if (self->recv_buf_.size() < hdr.length)
                break;
            if (self->on_frame_) {
                const byte* payload = self->recv_buf_.data() + server::FRAME_HEADER_SIZE;
                usize plen = hdr.length - server::FRAME_HEADER_SIZE;
                self->on_frame_(hdr.type, hdr.query_id, payload, plen);
            }
            self->recv_buf_.erase(self->recv_buf_.begin(), self->recv_buf_.begin() + hdr.length);
        }
        break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* qbuf = static_cast<QUIC_BUFFER*>(ev->SEND_COMPLETE.ClientContext);
        delete[] qbuf->Buffer;
        delete qbuf;
        break;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (!ev->SHUTDOWN_COMPLETE.AppCloseInProgress)
            self->api_->StreamClose(stream);
        self->stream_ = nullptr;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

} // namespace nyx::replication
