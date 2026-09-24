#include "server/wire.h"

#include <cstring>
#include <gtest/gtest.h>

using namespace nyx;
using namespace nyx::server;

// ---- encode/decode header ------------------------------------------------

TEST(WireTest, EncodeDecodeHeader) {
    std::vector<byte> buf;
    encode_header(buf, FrameType::QUERY, 42, 8);

    ASSERT_EQ(buf.size(), FRAME_HEADER_SIZE);

    FrameHeader h;
    ASSERT_TRUE(decode_header(buf.data(), buf.size(), h));
    EXPECT_EQ(h.type, FrameType::QUERY);
    EXPECT_EQ(h.query_id, 42u);
    EXPECT_EQ(h.length, FRAME_HEADER_SIZE + 8u);
}

TEST(WireTest, HeaderTruncatedReturnsFalse) {
    std::vector<byte> buf(FRAME_HEADER_SIZE - 1, byte{0});
    FrameHeader h;
    EXPECT_FALSE(decode_header(buf.data(), buf.size(), h));
}

TEST(WireTest, HeaderZeroLengthReturnsFalse) {
    FrameHeader h;
    EXPECT_FALSE(decode_header(nullptr, 0, h));
}

// ---- encode integers -------------------------------------------------------

TEST(WireTest, EncodeU8) {
    std::vector<byte> buf;
    encode_u8(buf, 0xAB);
    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 0xABu);
}

TEST(WireTest, EncodeU16LittleEndian) {
    std::vector<byte> buf;
    encode_u16(buf, 0x1234);
    ASSERT_EQ(buf.size(), 2u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 0x34u);
    EXPECT_EQ(static_cast<u8>(buf[1]), 0x12u);
}

TEST(WireTest, EncodeU32LittleEndian) {
    std::vector<byte> buf;
    encode_u32(buf, 0x12345678u);
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 0x78u);
    EXPECT_EQ(static_cast<u8>(buf[1]), 0x56u);
    EXPECT_EQ(static_cast<u8>(buf[2]), 0x34u);
    EXPECT_EQ(static_cast<u8>(buf[3]), 0x12u);
}

TEST(WireTest, EncodeU64LittleEndian) {
    std::vector<byte> buf;
    encode_u64(buf, 0x0102030405060708ULL);
    ASSERT_EQ(buf.size(), 8u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 0x08u);
    EXPECT_EQ(static_cast<u8>(buf[7]), 0x01u);
}

// ---- decode helpers --------------------------------------------------------

TEST(WireTest, DecodeU16) {
    byte data[2] = {byte{0x34}, byte{0x12}};
    EXPECT_EQ(decode_u16(data), 0x1234u);
}

TEST(WireTest, DecodeU32) {
    byte data[4] = {byte{0x78}, byte{0x56}, byte{0x34}, byte{0x12}};
    EXPECT_EQ(decode_u32(data), 0x12345678u);
}

TEST(WireTest, DecodeU64) {
    byte data[8] = {byte{0x08}, byte{0x07}, byte{0x06}, byte{0x05},
                    byte{0x04}, byte{0x03}, byte{0x02}, byte{0x01}};
    EXPECT_EQ(decode_u64(data), 0x0102030405060708ULL);
}

// ---- encode_str ------------------------------------------------------------

TEST(WireTest, EncodeStr) {
    std::vector<byte> buf;
    encode_str(buf, "hi");
    // 2-byte length LE + 2 chars
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 2u);
    EXPECT_EQ(static_cast<u8>(buf[1]), 0u);
    EXPECT_EQ(static_cast<char>(buf[2]), 'h');
    EXPECT_EQ(static_cast<char>(buf[3]), 'i');
}

TEST(WireTest, EncodeEmptyStr) {
    std::vector<byte> buf;
    encode_str(buf, "");
    ASSERT_EQ(buf.size(), 2u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 0u);
    EXPECT_EQ(static_cast<u8>(buf[1]), 0u);
}

// ---- encode_value ----------------------------------------------------------

TEST(WireTest, EncodeValueInt32) {
    std::vector<byte> buf;
    encode_value(buf, Value{i32(42)}, TypeId::INT32);
    ASSERT_EQ(buf.size(), 4u);
    i32 out;
    std::memcpy(&out, buf.data(), 4);
    EXPECT_EQ(out, 42);
}

TEST(WireTest, EncodeValueInt64) {
    std::vector<byte> buf;
    encode_value(buf, Value{i64(9999999)}, TypeId::INT64);
    ASSERT_EQ(buf.size(), 8u);
    i64 out;
    std::memcpy(&out, buf.data(), 8);
    EXPECT_EQ(out, 9999999);
}

TEST(WireTest, EncodeValueDouble) {
    std::vector<byte> buf;
    encode_value(buf, Value{f64(3.14)}, TypeId::DOUBLE);
    ASSERT_EQ(buf.size(), 8u);
    f64 out;
    std::memcpy(&out, buf.data(), 8);
    EXPECT_DOUBLE_EQ(out, 3.14);
}

TEST(WireTest, EncodeValueBool) {
    std::vector<byte> buf;
    encode_value(buf, Value{true}, TypeId::BOOL);
    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 1u);
}

TEST(WireTest, EncodeValueVarchar) {
    std::vector<byte> buf;
    encode_value(buf, Value{std::string("abc")}, TypeId::VARCHAR);
    // 2-byte length + 3 chars
    ASSERT_EQ(buf.size(), 5u);
    EXPECT_EQ(static_cast<u8>(buf[0]), 3u);
}

TEST(WireTest, EncodeValueDate) {
    std::vector<byte> buf;
    encode_value(buf, Value{Date{19723}}, TypeId::DATE);
    ASSERT_EQ(buf.size(), 4u);
    i32 days;
    std::memcpy(&days, buf.data(), 4);
    EXPECT_EQ(days, 19723);
}

TEST(WireTest, EncodeValueTimestamp) {
    std::vector<byte> buf;
    encode_value(buf, Value{Timestamp{123456789LL}}, TypeId::TIMESTAMP);
    ASSERT_EQ(buf.size(), 8u);
    i64 micros;
    std::memcpy(&micros, buf.data(), 8);
    EXPECT_EQ(micros, 123456789LL);
}

// ---- frame type round-trip -------------------------------------------------

TEST(WireTest, AllFrameTypes) {
    for (auto ft : {FrameType::AUTH_REQ, FrameType::AUTH_OK, FrameType::AUTH_ERR, FrameType::QUERY,
                    FrameType::RESULT_META, FrameType::RESULT_COL, FrameType::RESULT_END,
                    FrameType::QUERY_ERR}) {
        std::vector<byte> buf;
        encode_header(buf, ft, 1, 0);
        FrameHeader h;
        ASSERT_TRUE(decode_header(buf.data(), buf.size(), h));
        EXPECT_EQ(h.type, ft);
    }
}
