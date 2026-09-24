#pragma once

#include "common/result.h"
#include "common/types.h"
#include "database/database.h"
#include "replication/replication_config.h"
#include "replication/replication_manager.h"
#include "server/coordinator.h"

#include <memory>
#include <msquic.h>
#include <string>

namespace nyx::server {

class Server {
  public:
    static Result<Server> create(const std::string& data_dir, u16 port, const std::string& token,
                                 replication::NodeConfig node_cfg = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;

    Result<void> start();
    Result<void> run();
    bool is_leader() const { return !repl_mgr_ || repl_mgr_->is_leader(); }

  private:
    Server() = default;

    static QUIC_STATUS QUIC_API listener_cb_(HQUIC listener, void* ctx, QUIC_LISTENER_EVENT* ev);
    static QUIC_STATUS QUIC_API connection_cb_(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev);
    static QUIC_STATUS QUIC_API stream_cb_(HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev);

    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    std::unique_ptr<Database> db_;
    std::unique_ptr<Coordinator> coordinator_;
    std::unique_ptr<replication::ReplicationManager> repl_mgr_;
    std::string token_;
    u16 port_ = 0;
};

} // namespace nyx::server
