#pragma once

#include "common/types.h"
#include "server/wire.h"

#include <msquic.h>
#include <string>
#include <vector>

namespace nyx::replication {
class ReplicationManager;
}

namespace nyx::server {

class ReplicationSession {
  public:
    ReplicationSession(replication::ReplicationManager* repl, HQUIC conn,
                       const QUIC_API_TABLE* api);

    void on_stream(HQUIC stream);
    void on_data(const byte* data, usize len);
    void close();

  private:
    void dispatch_(FrameType type, u32 query_id, const byte* payload, usize len);
    void send_frame_(std::vector<byte> buf);

    replication::ReplicationManager* repl_;
    HQUIC connection_;
    HQUIC stream_ = nullptr;
    const QUIC_API_TABLE* api_;
    std::string node_id_;
    std::vector<byte> recv_buf_;
};

} // namespace nyx::server
