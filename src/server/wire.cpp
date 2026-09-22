#include "server/wire.h"

#include <cstring>

namespace nyx::server {

void encode_u8(std::vector<byte>& buf, u8 v) {
    buf.push_back(static_cast<byte>(v));
}

void encode_u16(std::vector<byte>& buf, u16 v) {
    buf.push_back(static_cast<byte>(v & 0xFFu));
    buf.push_back(static_cast<byte>((v >> 8) & 0xFFu));
}

void encode_u32(std::vector<byte>& buf, u32 v) {
    buf.push_back(static_cast<byte>(v & 0xFFu));
    buf.push_back(static_cast<byte>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<byte>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<byte>((v >> 24) & 0xFFu));
}

void encode_u64(std::vector<byte>& buf, u64 v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<byte>((v >> (i * 8)) & 0xFFu));
}

void encode_str(std::vector<byte>& buf, std::string_view s) {
    encode_u16(buf, static_cast<u16>(s.size()));
    for (char c : s)
        buf.push_back(static_cast<byte>(c));
}

void encode_value(std::vector<byte>& buf, const Value& v, TypeId t) {
    switch (t) {
    case TypeId::INT32: {
        i32 val = std::get<i32>(v);
        byte tmp[4];
        std::memcpy(tmp, &val, 4);
        buf.insert(buf.end(), tmp, tmp + 4);
        break;
    }
    case TypeId::INT64: {
        i64 val = std::get<i64>(v);
        byte tmp[8];
        std::memcpy(tmp, &val, 8);
        buf.insert(buf.end(), tmp, tmp + 8);
        break;
    }
    case TypeId::DOUBLE: {
        f64 val = std::get<f64>(v);
        byte tmp[8];
        std::memcpy(tmp, &val, 8);
        buf.insert(buf.end(), tmp, tmp + 8);
        break;
    }
    case TypeId::BOOL:
        encode_u8(buf, std::get<bool>(v) ? 1u : 0u);
        break;
    case TypeId::VARCHAR:
        encode_str(buf, std::get<std::string>(v));
        break;
    case TypeId::DATE: {
        i32 days = std::get<Date>(v).days;
        byte tmp[4];
        std::memcpy(tmp, &days, 4);
        buf.insert(buf.end(), tmp, tmp + 4);
        break;
    }
    case TypeId::TIMESTAMP: {
        i64 micros = std::get<Timestamp>(v).micros;
        byte tmp[8];
        std::memcpy(tmp, &micros, 8);
        buf.insert(buf.end(), tmp, tmp + 8);
        break;
    }
    default:
        break;
    }
}

void encode_header(std::vector<byte>& buf, FrameType t, u32 qid, u32 payload_len) {
    u32 total = static_cast<u32>(FRAME_HEADER_SIZE) + payload_len;
    encode_u32(buf, total);
    encode_u8(buf, static_cast<u8>(t));
    encode_u32(buf, qid);
}

bool decode_header(const byte* data, usize len, FrameHeader& out) {
    if (len < FRAME_HEADER_SIZE)
        return false;
    u32 length = 0;
    for (int i = 0; i < 4; ++i)
        length |= static_cast<u32>(static_cast<u8>(data[i])) << (i * 8);
    out.length = length;
    out.type = static_cast<FrameType>(static_cast<u8>(data[4]));
    u32 qid = 0;
    for (int i = 0; i < 4; ++i)
        qid |= static_cast<u32>(static_cast<u8>(data[5 + i])) << (i * 8);
    out.query_id = qid;
    return true;
}

u16 decode_u16(const byte* p) {
    return static_cast<u16>(static_cast<u8>(p[0])) | (static_cast<u16>(static_cast<u8>(p[1])) << 8);
}

u32 decode_u32(const byte* p) {
    u32 v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<u32>(static_cast<u8>(p[i])) << (i * 8);
    return v;
}

u64 decode_u64(const byte* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<u64>(static_cast<u8>(p[i])) << (i * 8);
    return v;
}

} // namespace nyx::server
