#include "replication/wal_streamer.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace nyx;
using namespace nyx::replication;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_wal_streamer_test";

class WalStreamerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
    }
    void TearDown() override { fs::remove_all(ROOT); }

    std::string make_wal(const std::vector<byte>& data) {
        std::string p = ROOT + "/test.wal";
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        return p;
    }

    std::string make_wal(usize size, byte fill = 0xAB) {
        return make_wal(std::vector<byte>(size, fill));
    }
};

TEST_F(WalStreamerTest, CurrentSizeMissingFile) {
    WalStreamer s(ROOT + "/nope.wal");
    EXPECT_EQ(s.current_size(), 0u);
}

TEST_F(WalStreamerTest, CurrentSize) {
    auto p = make_wal(1024);
    WalStreamer s(p);
    EXPECT_EQ(s.current_size(), 1024u);
}

TEST_F(WalStreamerTest, GetBatchFromStart) {
    auto p = make_wal(100);
    WalStreamer s(p);
    auto r = s.get_batch(0);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 100u);
}

TEST_F(WalStreamerTest, GetBatchFromMidpoint) {
    auto p = make_wal(100);
    WalStreamer s(p);
    auto r = s.get_batch(50);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 50u);
}

TEST_F(WalStreamerTest, GetBatchBeyondEnd) {
    auto p = make_wal(100);
    WalStreamer s(p);
    auto r = s.get_batch(200);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(WalStreamerTest, GetBatchMaxBytesLimit) {
    auto p = make_wal(10000);
    WalStreamer s(p);
    auto r = s.get_batch(0, 256);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 256u);
}

TEST_F(WalStreamerTest, GetBatchContents) {
    std::vector<byte> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto p = make_wal(data);
    WalStreamer s(p);
    auto r = s.get_batch(2);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 3u);
    EXPECT_EQ(r.value()[0], byte{0x03});
    EXPECT_EQ(r.value()[1], byte{0x04});
    EXPECT_EQ(r.value()[2], byte{0x05});
}

TEST_F(WalStreamerTest, GetBatchMissingFile) {
    WalStreamer s(ROOT + "/nope.wal");
    auto r = s.get_batch(0);
    EXPECT_FALSE(r.is_ok());
}
