#include "server/server.h"

#include "server/session.h"

#include <cstring>
#include <unistd.h>

namespace nyx::server {

static const QUIC_REGISTRATION_CONFIG reg_config = {"nyxdb", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
static const QUIC_BUFFER alpn = {5, (uint8_t*)"nyxdb"};

struct ConnectionCtx {
    Database*             db;
    const std::string*    token;
    const QUIC_API_TABLE* api;
    Session*              session = nullptr;
};

Result<Server> Server::create(const std::string& data_dir, u16 port,
                               const std::string& token) {
    Server s;
    s.token_ = token;
    s.port_  = port;

    if (QUIC_FAILED(MsQuicOpen2(&s.api_)))
        return Result<Server>::err("MsQuicOpen2 failed");

    if (QUIC_FAILED(s.api_->RegistrationOpen(&reg_config, &s.registration_)))
        return Result<Server>::err("RegistrationOpen failed");

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs      = 30000;
    settings.IsSet.IdleTimeoutMs = 1;

    if (QUIC_FAILED(s.api_->ConfigurationOpen(s.registration_, &alpn, 1, &settings,
                                               sizeof(settings), nullptr,
                                               &s.configuration_)))
        return Result<Server>::err("ConfigurationOpen failed");

    QUIC_CREDENTIAL_CONFIG server_cred{};
    server_cred.Type  = QUIC_CREDENTIAL_TYPE_NONE;
    server_cred.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    if (QUIC_FAILED(s.api_->ConfigurationLoadCredential(s.configuration_, &server_cred)))
        return Result<Server>::err("ConfigurationLoadCredential failed");

    auto db_r = Database::open(data_dir);
    if (!db_r.is_ok())
        return Result<Server>::err("Database::open: " + db_r.error().message);
    s.db_ = std::make_unique<Database>(std::move(db_r.value()));

    if (QUIC_FAILED(s.api_->ListenerOpen(s.registration_, listener_cb_, &s, &s.listener_)))
        return Result<Server>::err("ListenerOpen failed");

    return Result<Server>::ok(std::move(s));
}

Result<void> Server::run() {
    QUIC_ADDR addr{};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, port_);

    if (QUIC_FAILED(api_->ListenerStart(listener_, &alpn, 1, &addr)))
        return Result<void>::err("ListenerStart failed");

    for (;;)
        sleep(1);

    return Result<void>::ok();
}

Server::~Server() {
    if (listener_)      api_->ListenerClose(listener_);
    if (configuration_) api_->ConfigurationClose(configuration_);
    if (registration_)  api_->RegistrationClose(registration_);
    if (api_)           MsQuicClose(api_);
}

Server::Server(Server&& o) noexcept
    : api_(o.api_), registration_(o.registration_), configuration_(o.configuration_),
      listener_(o.listener_), db_(std::move(o.db_)), token_(std::move(o.token_)),
      port_(o.port_) {
    o.api_ = nullptr; o.registration_ = nullptr;
    o.configuration_ = nullptr; o.listener_ = nullptr;
}

Server& Server::operator=(Server&& o) noexcept {
    if (this != &o) {
        this->~Server();
        new (this) Server(std::move(o));
    }
    return *this;
}

QUIC_STATUS QUIC_API Server::listener_cb_(HQUIC, void* ctx, QUIC_LISTENER_EVENT* ev) {
    auto* self = static_cast<Server*>(ctx);
    if (ev->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        HQUIC conn = ev->NEW_CONNECTION.Connection;

        auto* cctx        = new ConnectionCtx;
        cctx->db          = self->db_.get();
        cctx->token       = &self->token_;
        cctx->api         = self->api_;

        self->api_->SetCallbackHandler(conn, (void*)connection_cb_, cctx);
        self->api_->ConnectionSetConfiguration(conn, self->configuration_);
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_NOT_SUPPORTED;
}

QUIC_STATUS QUIC_API Server::connection_cb_(HQUIC conn, void* ctx,
                                             QUIC_CONNECTION_EVENT* ev) {
    auto* cctx = static_cast<ConnectionCtx*>(ctx);
    switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = ev->PEER_STREAM_STARTED.Stream;
        cctx->session = new Session(cctx->db, *cctx->token, conn, cctx->api);
        cctx->session->on_stream(stream);
        cctx->api->SetCallbackHandler(stream, (void*)stream_cb_, cctx->session);
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        delete cctx->session;
        delete cctx;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API Server::stream_cb_(HQUIC, void* ctx, QUIC_STREAM_EVENT* ev) {
    auto* session = static_cast<Session*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        for (u32 i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            session->on_data(
                reinterpret_cast<const byte*>(ev->RECEIVE.Buffers[i].Buffer),
                ev->RECEIVE.Buffers[i].Length);
        }
        break;
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

} // namespace nyx::server
