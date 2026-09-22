#include "server/server.h"

#include "server/replication_session.h"
#include "server/session.h"

#include <cstdlib>
#include <cstring>
#include <glob.h>

namespace nyx::server {

static const QUIC_REGISTRATION_CONFIG reg_config = {"nyxdb", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
static const QUIC_BUFFER alpn = {5, (uint8_t*)"nyxdb"};

struct PendingSession {
    enum class State { WaitAuth, WaitType };
    State state = State::WaitAuth;
    std::vector<byte> buf;

    Session* client_session = nullptr;
    ReplicationSession* repl_session = nullptr;

    Database* db;
    const std::string* token;
    const QUIC_API_TABLE* api;
    replication::ReplicationManager* repl_mgr;
    HQUIC connection;
    HQUIC stream = nullptr;

    void on_stream(HQUIC s) { stream = s; }

    void on_data(const byte* data, usize len) {
        if (client_session) {
            client_session->on_data(data, len);
            return;
        }
        if (repl_session) {
            repl_session->on_data(data, len);
            return;
        }

        buf.insert(buf.end(), data, data + len);

        while (true) {
            FrameHeader hdr;
            if (!decode_header(buf.data(), buf.size(), hdr))
                break;
            if (buf.size() < hdr.length)
                break;

            const byte* payload = buf.data() + FRAME_HEADER_SIZE;
            usize payload_len = hdr.length - FRAME_HEADER_SIZE;

            if (state == State::WaitAuth) {
                if (hdr.type == FrameType::AUTH_REQ && payload_len >= 2) {
                    u16 tlen = decode_u16(payload);
                    if (payload_len >= static_cast<usize>(2 + tlen)) {
                        std::string tok(reinterpret_cast<const char*>(payload + 2), tlen);
                        if (tok == *token) {
                            std::vector<byte> resp;
                            encode_header(resp, FrameType::AUTH_OK, hdr.query_id, 0);
                            send_raw_(std::move(resp));
                            state = State::WaitType;
                        } else {
                            std::vector<byte> resp;
                            std::string_view emsg = "invalid token";
                            encode_header(resp, FrameType::AUTH_ERR, hdr.query_id,
                                          static_cast<u32>(2 + emsg.size()));
                            encode_str(resp, emsg);
                            send_raw_(std::move(resp));
                        }
                    }
                }
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(hdr.length));
                continue;
            }

            bool is_repl = (static_cast<u8>(hdr.type) >= static_cast<u8>(FrameType::REPL_HELLO));
            if (is_repl) {
                repl_session = new ReplicationSession(repl_mgr, connection, api);
                repl_session->on_stream(stream);
                repl_session->on_data(buf.data(), buf.size());
                buf.clear();
            } else {
                client_session = new Session(db, *token, connection, api, repl_mgr,
                                             /*pre_authed=*/true);
                client_session->on_stream(stream);
                client_session->on_data(buf.data(), buf.size());
                buf.clear();
            }
            return;
        }
    }

    void send_raw_(std::vector<byte> outbuf) {
        if (!stream)
            return;
        usize n = outbuf.size();
        byte* raw = new byte[n];
        std::memcpy(raw, outbuf.data(), n);
        QUIC_BUFFER* qbuf = new QUIC_BUFFER;
        qbuf->Buffer = raw;
        qbuf->Length = static_cast<u32>(n);
        api->StreamSend(stream, qbuf, 1, QUIC_SEND_FLAG_NONE, qbuf);
    }
};

struct ConnectionCtx {
    Database* db;
    const std::string* token;
    const QUIC_API_TABLE* api;
    replication::ReplicationManager* repl_mgr;
    PendingSession* pending = nullptr;
};

Result<Server> Server::create(const std::string& data_dir, u16 port, const std::string& token,
                              replication::NodeConfig node_cfg) {
    Server s;
    s.token_ = token;
    s.port_ = port;

    if (QUIC_FAILED(MsQuicOpen2(&s.api_)))
        return Result<Server>::err("MsQuicOpen2 failed");

    if (QUIC_FAILED(s.api_->RegistrationOpen(&reg_config, &s.registration_)))
        return Result<Server>::err("RegistrationOpen failed");

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 30000;
    settings.IsSet.IdleTimeoutMs = 1;
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.PeerBidiStreamCount = 1;

    if (QUIC_FAILED(s.api_->ConfigurationOpen(s.registration_, &alpn, 1, &settings,
                                              sizeof(settings), nullptr, &s.configuration_)))
        return Result<Server>::err("ConfigurationOpen failed");

    glob_t g{};
    std::string cert_path, key_path;
    if (glob("/tmp/quictest.*/localhost_ss_cert.pem", 0, nullptr, &g) == 0 && g.gl_pathc > 0) {
        cert_path = g.gl_pathv[0];
        key_path = cert_path.substr(0, cert_path.rfind('/') + 1) + "localhost_ss_key.pem";
        globfree(&g);
    } else {
        globfree(&g);
        cert_path = "/tmp/nyxdb-server-cert.pem";
        key_path = "/tmp/nyxdb-server-key.pem";
        int rc = std::system("openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes "
                             "-keyout /tmp/nyxdb-server-key.pem "
                             "-out /tmp/nyxdb-server-cert.pem "
                             "-subj \"/CN=nyxdb\" 2>/dev/null");
        if (rc != 0)
            return Result<Server>::err("failed to generate TLS cert (openssl not found?)");
    }

    QUIC_CERTIFICATE_FILE cert_file;
    cert_file.CertificateFile = cert_path.c_str();
    cert_file.PrivateKeyFile = key_path.c_str();

    QUIC_CREDENTIAL_CONFIG server_cred{};
    server_cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    server_cred.CertificateFile = &cert_file;
    server_cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    if (QUIC_FAILED(s.api_->ConfigurationLoadCredential(s.configuration_, &server_cred)))
        return Result<Server>::err("ConfigurationLoadCredential failed");

    auto db_r = Database::open(data_dir);
    if (!db_r.is_ok())
        return Result<Server>::err("Database::open: " + db_r.error().message);
    s.db_ = std::make_unique<Database>(std::move(db_r.value()));

    if (node_cfg.role != replication::NodeConfig::Role::Standalone) {
        s.repl_mgr_ = replication::ReplicationManager::create(node_cfg, s.db_.get(), data_dir);
        s.db_->set_compaction_gate(
            [rm = s.repl_mgr_.get()]() { return rm->safe_compaction_lsn(); });
        s.repl_mgr_->start();
    }

    return Result<Server>::ok(std::move(s));
}

Result<void> Server::start() {
    if (QUIC_FAILED(api_->ListenerOpen(registration_, listener_cb_, this, &listener_)))
        return Result<void>::err("ListenerOpen failed");

    QUIC_ADDR addr{};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, port_);

    if (QUIC_FAILED(api_->ListenerStart(listener_, &alpn, 1, &addr)))
        return Result<void>::err("ListenerStart failed");

    return Result<void>::ok();
}

Result<void> Server::run() {
    auto r = start();
    if (!r.is_ok())
        return r;

    for (;;)
        sleep(1);

    return Result<void>::ok();
}

Server::~Server() {
    if (repl_mgr_)
        repl_mgr_->stop();
    if (listener_)
        api_->ListenerClose(listener_);
    if (configuration_)
        api_->ConfigurationClose(configuration_);
    if (registration_)
        api_->RegistrationClose(registration_);
    if (api_)
        MsQuicClose(api_);
}

Server::Server(Server&& o) noexcept
    : api_(o.api_), registration_(o.registration_), configuration_(o.configuration_),
      listener_(o.listener_), db_(std::move(o.db_)), repl_mgr_(std::move(o.repl_mgr_)),
      token_(std::move(o.token_)), port_(o.port_) {
    o.api_ = nullptr;
    o.registration_ = nullptr;
    o.configuration_ = nullptr;
    o.listener_ = nullptr;
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

        auto* cctx = new ConnectionCtx;
        cctx->db = self->db_.get();
        cctx->token = &self->token_;
        cctx->api = self->api_;
        cctx->repl_mgr = self->repl_mgr_.get();

        self->api_->SetCallbackHandler(conn, (void*)connection_cb_, cctx);
        self->api_->ConnectionSetConfiguration(conn, self->configuration_);
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_NOT_SUPPORTED;
}

QUIC_STATUS QUIC_API Server::connection_cb_(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev) {
    auto* cctx = static_cast<ConnectionCtx*>(ctx);
    switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = ev->PEER_STREAM_STARTED.Stream;
        cctx->pending = new PendingSession;
        cctx->pending->db = cctx->db;
        cctx->pending->token = cctx->token;
        cctx->pending->api = cctx->api;
        cctx->pending->repl_mgr = cctx->repl_mgr;
        cctx->pending->connection = conn;
        cctx->pending->on_stream(stream);
        cctx->api->SetCallbackHandler(stream, (void*)stream_cb_, cctx);
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        if (cctx->pending) {
            delete cctx->pending->client_session;
            delete cctx->pending->repl_session;
            delete cctx->pending;
        }
        cctx->api->ConnectionClose(conn);
        delete cctx;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API Server::stream_cb_(HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev) {
    auto* cctx = static_cast<ConnectionCtx*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        if (cctx->pending) {
            for (u32 i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                cctx->pending->on_data(reinterpret_cast<const byte*>(ev->RECEIVE.Buffers[i].Buffer),
                                       ev->RECEIVE.Buffers[i].Length);
            }
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
            cctx->api->StreamClose(stream);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

} // namespace nyx::server
