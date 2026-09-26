#include "server/shard_client.h"

#include "replication/quic_client.h"
#include "replication/replication_config.h"
#include "server/wire.h"
#include "storage/disk/schema.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>

namespace nyx::server {

namespace {

struct FrameQ {
    std::mutex mu;
    std::condition_variable cv;
    std::queue<std::pair<FrameType, std::vector<byte>>> q;

    std::optional<std::pair<FrameType, std::vector<byte>>> wait(std::chrono::seconds t) {
        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, t, [this] { return !q.empty(); }))
            return std::nullopt;
        auto fr = std::move(q.front());
        q.pop();
        return fr;
    }
};

static Value decode_col_value(const byte* p, TypeId t, usize& off, usize avail) {
    switch (t) {
    case TypeId::INT32: {
        if (off + 4 > avail)
            return std::monostate{};
        i32 v;
        std::memcpy(&v, p + off, 4);
        off += 4;
        return v;
    }
    case TypeId::INT64: {
        if (off + 8 > avail)
            return std::monostate{};
        i64 v;
        std::memcpy(&v, p + off, 8);
        off += 8;
        return v;
    }
    case TypeId::DOUBLE: {
        if (off + 8 > avail)
            return std::monostate{};
        f64 v;
        std::memcpy(&v, p + off, 8);
        off += 8;
        return v;
    }
    case TypeId::BOOL: {
        if (off + 1 > avail)
            return std::monostate{};
        bool v = p[off++] != 0;
        return v;
    }
    case TypeId::VARCHAR: {
        if (off + 2 > avail)
            return std::monostate{};
        u16 slen = decode_u16(p + off);
        off += 2;
        if (off + slen > avail)
            return std::monostate{};
        std::string s(reinterpret_cast<const char*>(p + off), slen);
        off += slen;
        return s;
    }
    case TypeId::DATE: {
        if (off + 4 > avail)
            return std::monostate{};
        i32 days;
        std::memcpy(&days, p + off, 4);
        off += 4;
        return Date{days};
    }
    case TypeId::TIMESTAMP: {
        if (off + 8 > avail)
            return std::monostate{};
        i64 micros;
        std::memcpy(&micros, p + off, 8);
        off += 8;
        return Timestamp{micros};
    }
    default:
        return std::monostate{};
    }
}

} // namespace

ShardClient::ShardClient(std::unique_ptr<replication::QuicClient> conn, std::string addr,
                         std::string token)
    : conn_(std::move(conn)), addr_(std::move(addr)), token_(std::move(token)) {}

Result<ShardClient> ShardClient::connect(const std::string& addr, const std::string& token) {
    auto fq = std::make_shared<FrameQ>();

    auto cr = replication::QuicClient::create();
    if (!cr.is_ok())
        return Result<ShardClient>::err("shard_client: create failed: " + cr.error().message);

    auto& client = *cr.value();
    client.set_frame_handler([fq](FrameType t, u32, const byte* d, usize l) {
        std::lock_guard<std::mutex> lk(fq->mu);
        fq->q.push({t, std::vector<byte>(d, d + l)});
        fq->cv.notify_one();
    });

    auto [host, port] = replication::parse_node_addr(addr);
    if (client.connect(host, port).is_err())
        return Result<ShardClient>::err("shard_client: connect to " + addr + " failed");

    std::vector<byte> auth;
    encode_str(auth, token);
    client.send_frame(FrameType::AUTH_REQ, 1, auth);

    auto auth_r = fq->wait(std::chrono::seconds(5));
    if (!auth_r || auth_r->first != FrameType::AUTH_OK)
        return Result<ShardClient>::err("shard_client: auth failed for " + addr);

    return Result<ShardClient>::ok(ShardClient(std::move(cr.value()), addr, token));
}

Result<ExecuteResult> ShardClient::execute(const std::string& sql) {
    auto fq = std::make_shared<FrameQ>();
    conn_->set_frame_handler([fq](FrameType t, u32, const byte* d, usize l) {
        std::lock_guard<std::mutex> lk(fq->mu);
        fq->q.push({t, std::vector<byte>(d, d + l)});
        fq->cv.notify_one();
    });

    std::vector<byte> qpay;
    encode_str(qpay, sql);
    conn_->send_frame(FrameType::QUERY, 1, qpay);

    ExecuteResult result;
    Schema& schema = result.schema;
    bool got_meta = false;
    bool got_end = false;

    while (!got_end) {
        auto fr = fq->wait(std::chrono::seconds(30));
        if (!fr)
            return Result<ExecuteResult>::err("shard_client: timeout waiting for result from " +
                                              addr_);

        const auto& payload = fr->second;
        const byte* p = payload.data();
        usize plen = payload.size();

        switch (fr->first) {
        case FrameType::QUERY_ERR: {
            if (plen < 2)
                return Result<ExecuteResult>::err("shard query error");
            u16 mlen = decode_u16(p);
            std::string msg(reinterpret_cast<const char*>(p + 2),
                            std::min(static_cast<usize>(mlen), plen - 2));
            return Result<ExecuteResult>::err(msg);
        }
        case FrameType::RESULT_META: {
            if (plen < 2)
                break;
            u16 ncols = decode_u16(p);
            usize off = 2;
            schema.clear();
            result.columns.clear();
            result.columns.resize(ncols);
            for (u16 c = 0; c < ncols; ++c) {
                if (off + 1 > plen)
                    break;
                u8 nlen = static_cast<u8>(p[off++]);
                if (off + nlen > plen)
                    break;
                std::string cname(reinterpret_cast<const char*>(p + off), nlen);
                off += nlen;
                if (off + 2 > plen)
                    break;
                auto tid = static_cast<TypeId>(static_cast<u8>(p[off++]));
                bool nullable = p[off++] != 0;
                schema.push_back({cname, tid, nullable, 255, std::nullopt});
            }
            got_meta = true;
            break;
        }
        case FrameType::RESULT_COL: {
            if (!got_meta || plen < 10)
                break;
            u16 col_idx = decode_u16(p);
            u64 row_count = decode_u64(p + 2);
            usize off = 10;
            if (col_idx >= static_cast<u16>(schema.size()))
                break;
            TypeId t = schema[col_idx].type;
            for (u64 r = 0; r < row_count; ++r) {
                if (off >= plen)
                    break;
                bool is_null = p[off++] != 0;
                if (is_null)
                    result.columns[col_idx].push_back(std::monostate{});
                else
                    result.columns[col_idx].push_back(decode_col_value(p, t, off, plen));
            }
            break;
        }
        case FrameType::RESULT_END: {
            if (plen >= 8)
                result.rows_affected = decode_u64(p);
            got_end = true;
            break;
        }
        default:
            break;
        }
    }

    return Result<ExecuteResult>::ok(std::move(result));
}

Result<void> ShardClient::reconnect() {
    conn_->close();
    auto r = ShardClient::connect(addr_, token_);
    if (!r.is_ok())
        return Result<void>::err(r.error().message);
    *this = std::move(r.value());
    return Result<void>::ok();
}

} // namespace nyx::server
