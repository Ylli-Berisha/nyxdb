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
};

TEST_F(BufferPoolTest, NewPageReturnsFreshPage) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(4, 2, dm);

    PageId id = 0;
    auto res = bp.new_page(id);
    ASSERT_TRUE(res.is_ok());
    EXPECT_EQ(id, 0u);
    EXPECT_EQ(res.value()->page_id(), id);
    EXPECT_EQ(res.value()->pin_count, 1);
}

TEST_F(BufferPoolTest, FetchHitReturnsSamePointer) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(4, 2, dm);

    PageId id;
    auto n = bp.new_page(id);
    ASSERT_TRUE(n.is_ok());
    Page* p1 = n.value();
    ASSERT_TRUE(bp.unpin_page(id, false).is_ok());

    auto f = bp.fetch_page(id);
    ASSERT_TRUE(f.is_ok());
    EXPECT_EQ(f.value(), p1);
    ASSERT_TRUE(bp.unpin_page(id, false).is_ok());
}

TEST_F(BufferPoolTest, UnpinDirtyMigratesToDirtyPool) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(4, 2, dm);

    PageId id;
    auto n = bp.new_page(id);
    ASSERT_TRUE(n.is_ok());
    std::memset(n.value()->payload(), 0x77, PAGE_PAYLOAD_SIZE);
    ASSERT_TRUE(bp.unpin_page(id, true).is_ok());

    auto f = bp.fetch_page(id);
    ASSERT_TRUE(f.is_ok());
    EXPECT_EQ(f.value()->payload()[0], 0x77);
    ASSERT_TRUE(bp.unpin_page(id, false).is_ok());
}

TEST_F(BufferPoolTest, FlushAllPersistsData) {
    DiskManager dm(TEST_FILE);
    {
        BufferPool bp(4, 2, dm);
        PageId id;
        auto n = bp.new_page(id);
        ASSERT_TRUE(n.is_ok());
        std::memset(n.value()->payload(), 0x55, PAGE_PAYLOAD_SIZE);
        ASSERT_TRUE(bp.unpin_page(id, true).is_ok());
        ASSERT_TRUE(bp.flush_all().is_ok());
    }

    Page p{};
    ASSERT_TRUE(dm.read_page(0, p).is_ok());
    EXPECT_EQ(p.payload()[0], 0x55);
}

TEST_F(BufferPoolTest, EvictsCleanFreshPagesWhenPoolFull) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(2, 2, dm);

    PageId ids[3];
    for (int i = 0; i < 3; ++i) {
        auto n = bp.new_page(ids[i]);
        ASSERT_TRUE(n.is_ok());
        ASSERT_TRUE(bp.unpin_page(ids[i], false).is_ok()); // clean unpin
    }

    // First page must have been evicted; refetching re-reads from disk.
    auto f = bp.fetch_page(ids[0]);
    ASSERT_TRUE(f.is_ok());
    EXPECT_EQ(f.value()->page_id(), ids[0]);
    ASSERT_TRUE(bp.unpin_page(ids[0], false).is_ok());
}

TEST_F(BufferPoolTest, DirtyPoolRotationOnWatermark) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(8, 8, dm); // dirty capacity 8 -> halves of 4 each

    // Fill enough dirty pages to trip the 75% high watermark on the active half
    // (4-slot half, watermark_low_free = 1 -> rotate when 3 slots used).
    for (int i = 0; i < 6; ++i) {
        PageId id;
        auto n = bp.new_page(id);
        ASSERT_TRUE(n.is_ok());
        std::memset(n.value()->payload(), static_cast<byte>(i + 1), PAGE_PAYLOAD_SIZE);
        ASSERT_TRUE(bp.unpin_page(id, true).is_ok());
    }
    ASSERT_TRUE(bp.flush_all().is_ok());

    for (PageId id = 0; id < 6; ++id) {
        Page p{};
        ASSERT_TRUE(dm.read_page(id, p).is_ok());
        EXPECT_EQ(p.payload()[0], static_cast<byte>(id + 1));
    }
}

TEST_F(BufferPoolTest, UnpinNotInPoolErrors) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(4, 2, dm);

    auto res = bp.unpin_page(99, false);
    EXPECT_TRUE(res.is_err());
}

TEST_F(BufferPoolTest, DoubleUnpinErrors) {
    DiskManager dm(TEST_FILE);
    BufferPool bp(4, 2, dm);

    PageId id;
    ASSERT_TRUE(bp.new_page(id).is_ok());
    ASSERT_TRUE(bp.unpin_page(id, false).is_ok());
    auto second = bp.unpin_page(id, false);
    EXPECT_TRUE(second.is_err());
}
