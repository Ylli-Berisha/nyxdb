#include "server/replication_session.h"

#include "replication/replication_manager.h"

#include <cstring>

namespace nyx::server {

ReplicationSession::ReplicationSession(replication::ReplicationManager* repl, HQUIC conn,
                                       const QUIC_API_TABLE* api)
    : repl_(repl), connection_(conn), api_(api) {}

void ReplicationSession::on_stream(HQUIC stream) {
    stream_ = stream;
}

void ReplicationSession::on_data(const byte* data, usize len) {
    recv_buf_.insert(recv_buf_.end(), data, data + len);

    while (true) {
        FrameHeader hdr;
        if (!decode_header(recv_buf_.data(), recv_buf_.size(), hdr))
            break;
        if (recv_buf_.size() < hdr.length)
            break;

        const byte* payload = recv_buf_.data() + FRAME_HEADER_SIZE;
        usize payload_len = hdr.length - FRAME_HEADER_SIZE;

        dispatch_(hdr.type, hdr.query_id, payload, payload_len);

        recv_buf_.erase(recv_buf_.begin(),
                        recv_buf_.begin() + static_cast<std::ptrdiff_t>(hdr.length));
    }
}

void ReplicationSession::dispatch_(FrameType type, u32 /*query_id*/, const byte* payload,
                                   usize len) {
    if (type == FrameType::REPL_HELLO) {
        if (len < 2)
            return;
        u16 nlen = decode_u16(payload);
        if (len < static_cast<usize>(2 + nlen + 8))
            return;
        node_id_.assign(reinterpret_cast<const char*>(payload + 2), nlen);
        u64 confirmed_lsn = decode_u64(payload + 2 + nlen);
        if (repl_)
            repl_->on_hello(node_id_, confirmed_lsn);
        return;
    }

    // Election frames arrive on their own fire-and-forget connections (no REPL_HELLO).
    // Forward them to the ReplicationManager regardless of whether node_id_ is set.
    if (type == FrameType::REPL_HEARTBEAT || type == FrameType::REPL_VOTE_REQ ||
        type == FrameType::REPL_VOTE_RESP) {
        if (!repl_)
            return;
        auto reply = [this](FrameType t, const std::vector<byte>& p) {
            std::vector<byte> buf;
            encode_header(buf, t, 0, static_cast<u32>(p.size()));
            buf.insert(buf.end(), p.begin(), p.end());
            send_frame_(std::move(buf));
        };
        repl_->handle_repl_frame(node_id_, type, payload, len, reply);
        return;
    }

    if (!repl_)
        return;

    auto reply = [this](FrameType t, const std::vector<byte>& p) {
        std::vector<byte> buf;
        encode_header(buf, t, 0, static_cast<u32>(p.size()));
        buf.insert(buf.end(), p.begin(), p.end());
        send_frame_(std::move(buf));
    };

    repl_->handle_repl_frame(node_id_, type, payload, len, reply);
}

void ReplicationSession::send_frame_(std::vector<byte> buf) {
    if (!stream_)
        return;

    usize n = buf.size();
    byte* raw = new byte[n];
    std::memcpy(raw, buf.data(), n);

    QUIC_BUFFER* qbuf = new QUIC_BUFFER;
    qbuf->Buffer = raw;
    qbuf->Length = static_cast<u32>(n);

    api_->StreamSend(stream_, qbuf, 1, QUIC_SEND_FLAG_NONE, qbuf);
}

void ReplicationSession::close() {
    if (connection_)
        api_->ConnectionShutdown(connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
}

} // namespace nyx::server
