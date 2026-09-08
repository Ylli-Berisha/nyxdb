#include "storage/disk/disk_manager.h"
#include "storage/memory/buffer_pool.h"

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_FILE = "/tmp/nyxdb_bp_test.col";

class BufferPoolTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove(TEST_FILE); }
    void TearDown() override { fs::remove(TEST_FILE); }

    static PageId write_page_with_marker(DiskManager& dm, byte marker) {
        auto id_res = dm.allocate_page();
        EXPECT_TRUE(id_res.is_ok());
        PageId id = id_res.value();
        Page p{};
        p.reset(id);
        std::memset(p.payload(), marker, PAGE_PAYLOAD_SIZE);
        EXPECT_TRUE(dm.write_page(p).is_ok());
        return id;
    }
};

TEST_F(BufferPoolTest, FetchReadsPageFromDisk) {
    DiskManager dm(TEST_FILE);
    PageId id = write_page_with_marker(dm, 0x33);

    BufferPool bp(4, dm);
    auto f = bp.fetch_page(id);
    ASSERT_TRUE(f.is_ok());
    EXPECT_EQ(f.value()->page_id(), id);
    EXPECT_EQ(f.value()->payload()[0], static_cast<byte>(0x33));
    EXPECT_EQ(f.value()->pin_count, 1);
    ASSERT_TRUE(bp.unpin_page(id).is_ok());
}

TEST_F(BufferPoolTest, FetchHitReturnsSamePointer) {
    DiskManager dm(TEST_FILE);
    PageId id = write_page_with_marker(dm, 0x44);

    BufferPool bp(4, dm);
    auto f1 = bp.fetch_page(id);
    ASSERT_TRUE(f1.is_ok());
    Page* p1 = f1.value();
    ASSERT_TRUE(bp.unpin_page(id).is_ok());

    auto f2 = bp.fetch_page(id);
    ASSERT_TRUE(f2.is_ok());
    EXPECT_EQ(f2.value(), p1);
    ASSERT_TRUE(bp.unpin_page(id).is_ok());
}

TEST_F(BufferPoolTest, EvictsCleanPagesWhenPoolFull) {
    DiskManager dm(TEST_FILE);
    PageId ids[3];
    for (int i = 0; i < 3; ++i)
        ids[i] = write_page_with_marker(dm, static_cast<byte>(0xa0 + i));

    BufferPool bp(2, dm);
    for (int i = 0; i < 3; ++i) {
        auto f = bp.fetch_page(ids[i]);
        ASSERT_TRUE(f.is_ok());
        ASSERT_TRUE(bp.unpin_page(ids[i]).is_ok());
    }

    auto f = bp.fetch_page(ids[0]);
    ASSERT_TRUE(f.is_ok());
    EXPECT_EQ(f.value()->page_id(), ids[0]);
    EXPECT_EQ(f.value()->payload()[0], static_cast<byte>(0xa0));
    ASSERT_TRUE(bp.unpin_page(ids[0]).is_ok());
}

TEST_F(BufferPoolTest, UnpinNotInPoolErrors) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(4, dm);

    auto res = bp.unpin_page(99);
    EXPECT_TRUE(res.is_err());
}

TEST_F(BufferPoolTest, DoubleUnpinErrors) {
    DiskManager dm(TEST_FILE);
    PageId id = write_page_with_marker(dm, 0x55);

    BufferPool bp(4, dm);
    ASSERT_TRUE(bp.fetch_page(id).is_ok());
    ASSERT_TRUE(bp.unpin_page(id).is_ok());
    auto second = bp.unpin_page(id);
    EXPECT_TRUE(second.is_err());
}
