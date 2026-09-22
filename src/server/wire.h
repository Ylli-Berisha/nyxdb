#pragma once

#include "common/types.h"
#include "storage/disk/type_id.h"
#include "storage/disk/value.h"

#include <string_view>
#include <vector>

namespace nyx::server {

enum class FrameType : u8 {
    AUTH_REQ = 0x01,
    AUTH_OK = 0x02,
    AUTH_ERR = 0x03,

    QUERY = 0x10,
    RESULT_META = 0x11,
    RESULT_COL = 0x12,
    RESULT_END = 0x13,
    QUERY_ERR = 0x14,

    REPL_HELLO = 0x20,
    REPL_WAL_PULL = 0x21,
    REPL_WAL_BATCH = 0x22,
    REPL_WAL_STALE = 0x23,
    REPL_WAL_ACK = 0x24,
    REPL_SNAPSHOT_REQ = 0x25,
    REPL_SNAPSHOT_META = 0x26,
    REPL_SNAPSHOT_DATA = 0x27,
    REPL_SNAPSHOT_END = 0x28,

    REPL_HEARTBEAT = 0x30,
    REPL_VOTE_REQ = 0x31,
    REPL_VOTE_RESP = 0x32,

    REPL_FORWARD_WRITE = 0x40,
    REPL_FORWARD_RESP = 0x41,
};

struct FrameHeader {
    u32 length;
    FrameType type;
    u32 query_id;
};

static constexpr usize FRAME_HEADER_SIZE = 9;

void encode_u8(std::vector<byte>& buf, u8 v);
void encode_u16(std::vector<byte>& buf, u16 v);
void encode_u32(std::vector<byte>& buf, u32 v);
void encode_u64(std::vector<byte>& buf, u64 v);
void encode_str(std::vector<byte>& buf, std::string_view s);
void encode_value(std::vector<byte>& buf, const Value& v, TypeId t);

void encode_header(std::vector<byte>& buf, FrameType t, u32 qid, u32 payload_len);

bool decode_header(const byte* data, usize len, FrameHeader& out);

u16 decode_u16(const byte* p);
u32 decode_u32(const byte* p);
u64 decode_u64(const byte* p);

} // namespace nyx::server
