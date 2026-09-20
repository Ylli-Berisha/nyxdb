#include "server/session.h"

#include <cstring>

namespace nyx::server {

Session::Session(Database* db, std::string token, HQUIC connection,
                 const QUIC_API_TABLE* api)
    : db_(db), token_(std::move(token)), connection_(connection), api_(api) {}

void Session::on_stream(HQUIC stream) {
    stream_ = stream;
}

void Session::on_data(const byte* data, usize len) {
    recv_buf_.insert(recv_buf_.end(), data, data + len);

    while (true) {
        FrameHeader hdr;
        if (!decode_header(recv_buf_.data(), recv_buf_.size(), hdr))
            break;
        if (recv_buf_.size() < hdr.length)
            break;

        const byte* payload     = recv_buf_.data() + FRAME_HEADER_SIZE;
        usize       payload_len = hdr.length - FRAME_HEADER_SIZE;

        switch (hdr.type) {
        case FrameType::AUTH_REQ:
            handle_auth_req_(payload, payload_len, hdr.query_id);
            break;
        case FrameType::QUERY:
            handle_query_(payload, payload_len, hdr.query_id);
            break;
        default:
            break;
        }

        recv_buf_.erase(recv_buf_.begin(),
                        recv_buf_.begin() + static_cast<std::ptrdiff_t>(hdr.length));
    }
}

void Session::handle_auth_req_(const byte* payload, usize payload_len, u32 qid) {
    if (authed_)
        return;
    if (payload_len < 2)
        return;

    u16 token_len = static_cast<u16>(payload[0]) | (static_cast<u16>(payload[1]) << 8);
    if (payload_len < static_cast<usize>(2 + token_len))
        return;

    std::string received(reinterpret_cast<const char*>(payload + 2), token_len);
    if (received == token_) {
        authed_ = true;
        std::vector<byte> buf;
        encode_header(buf, FrameType::AUTH_OK, qid, 0);
        send_frame_(std::move(buf));
    } else {
        send_err_(qid, FrameType::AUTH_ERR, "invalid token");
        close();
    }
}

void Session::handle_query_(const byte* payload, usize payload_len, u32 qid) {
    if (!authed_) {
        send_err_(qid, FrameType::AUTH_ERR, "not authenticated");
        close();
        return;
    }
    if (payload_len < 2)
        return;

    u16 sql_len = static_cast<u16>(payload[0]) | (static_cast<u16>(payload[1]) << 8);
    if (payload_len < static_cast<usize>(2 + sql_len))
        return;

    std::string sql(reinterpret_cast<const char*>(payload + 2), sql_len);
    auto r = db_->execute(sql);
    if (!r.is_ok()) {
        send_err_(qid, FrameType::QUERY_ERR, r.error().message);
        return;
    }
    send_result_(qid, r.value());
}

void Session::send_result_(u32 qid, const ExecuteResult& r) {
    const Schema& schema = r.schema;
    usize col_count = schema.size();

    {
        std::vector<byte> buf;
        usize payload_size = 2;
        for (const auto& col : schema)
            payload_size += 1 + col.name.size() + 1 + 1;
        encode_header(buf, FrameType::RESULT_META, qid, static_cast<u32>(payload_size));
        encode_u16(buf, static_cast<u16>(col_count));
        for (const auto& col : schema) {
            encode_u8(buf, static_cast<u8>(col.name.size()));
            for (char c : col.name)
                buf.push_back(static_cast<byte>(c));
            encode_u8(buf, static_cast<u8>(col.type));
            encode_u8(buf, col.nullable ? 1u : 0u);
        }
        send_frame_(std::move(buf));
    }

    u64 row_count = static_cast<u64>(r.row_count());

    for (usize ci = 0; ci < col_count; ++ci) {
        const auto& col_vals = r.columns[ci];
        TypeId t = schema[ci].type;

        std::vector<byte> buf;
        std::vector<byte> payload;
        encode_u16(payload, static_cast<u16>(ci));
        encode_u64(payload, row_count);
        for (const auto& v : col_vals) {
            if (std::holds_alternative<std::monostate>(v)) {
                encode_u8(payload, 1u);
            } else {
                encode_u8(payload, 0u);
                encode_value(payload, v, t);
            }
        }
        encode_header(buf, FrameType::RESULT_COL, qid, static_cast<u32>(payload.size()));
        buf.insert(buf.end(), payload.begin(), payload.end());
        send_frame_(std::move(buf));
    }

    {
        std::vector<byte> buf;
        encode_header(buf, FrameType::RESULT_END, qid, 8);
        encode_u64(buf, r.rows_affected);
        send_frame_(std::move(buf));
    }
}

void Session::send_err_(u32 qid, FrameType t, std::string_view msg) {
    std::vector<byte> buf;
    encode_header(buf, t, qid, static_cast<u32>(2 + msg.size()));
    encode_str(buf, msg);
    send_frame_(std::move(buf));
}

void Session::send_frame_(std::vector<byte> buf) {
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

void Session::close() {
    if (connection_)
        api_->ConnectionShutdown(connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
}

} // namespace nyx::server
