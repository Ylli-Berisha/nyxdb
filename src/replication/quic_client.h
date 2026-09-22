#pragma once

#include "common/result.h"
#include "common/types.h"
#include "server/wire.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <msquic.h>
#include <mutex>
#include <string>
#include <vector>

namespace nyx::replication {

class QuicClient {
  public:
    static Result<std::unique_ptr<QuicClient>> create();
    ~QuicClient();

    QuicClient(const QuicClient&) = delete;
    QuicClient& operator=(const QuicClient&) = delete;
    QuicClient(QuicClient&&) = delete;
    QuicClient& operator=(QuicClient&&) = delete;

    Result<void> connect(const std::string& host, u16 port);
    Result<void> send_frame(server::FrameType type, u32 query_id, const std::vector<byte>& payload);
    void set_frame_handler(std::function<void(server::FrameType, u32, const byte*, usize)> handler);
    void close();

  private:
    QuicClient() = default;

    static QUIC_STATUS QUIC_API connection_cb_(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev);
    static QUIC_STATUS QUIC_API stream_cb_(HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev);

    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC connection_ = nullptr;
    HQUIC stream_ = nullptr;

    std::function<void(server::FrameType, u32, const byte*, usize)> on_frame_;
    std::vector<byte> recv_buf_;

    std::mutex connect_mu_;
    std::condition_variable connect_cv_;
    bool stream_ready_ = false;
    bool connect_failed_ = false;
};

} // namespace nyx::replication
