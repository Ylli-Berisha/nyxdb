#pragma once

#include "common/types.h"
#include "database/database.h"
#include "server/wire.h"

#include <msquic.h>
#include <string>
#include <vector>

namespace nyx::server {

class Session {
  public:
    Session(Database* db, std::string token, HQUIC connection, const QUIC_API_TABLE* api);
    ~Session() = default;

    void on_stream(HQUIC stream);
    void on_data(const byte* data, usize len);
    void close();

  private:
    void handle_auth_req_(const byte* payload, usize payload_len, u32 qid);
    void handle_query_(const byte* payload, usize payload_len, u32 qid);

    void send_frame_(std::vector<byte> buf);
    void send_result_(u32 qid, const ExecuteResult& r);
    void send_err_(u32 qid, FrameType t, std::string_view msg);

    Database* db_;
    std::string token_;
    HQUIC connection_;
    HQUIC stream_ = nullptr;
    const QUIC_API_TABLE* api_;
    bool authed_ = false;

    std::vector<byte> recv_buf_;
};

} // namespace nyx::server
