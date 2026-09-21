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

} // namespace nyx::server
