#include "storage/disk/segment.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_segment_test";
static constexpr u64 THRESH = 4;

class SegmentTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(ROOT); }
    void TearDown() override { fs::remove_all(ROOT); }

    static Schema two_col() {
        return {{"id", TypeId::INT32, false}, {"val", TypeId::INT32, false}};
    }

    static std::vector<Value> row(i32 id, i32 val) { return {id, val}; }
};

TEST_F(SegmentTest, FlushThresholdSealsSegment) {
    auto res = Table::create(ROOT, "t", two_col(), THRESH);
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    for (i32 i = 0; i < static_cast<i32>(THRESH); ++i) {
        auto r = t.insert(row(i, i * 10));
        ASSERT_TRUE(r.is_ok());
    }

    EXPECT_EQ(t.segments().size(), 1u);
    EXPECT_EQ(t.wb_base_row_id(), THRESH);
    EXPECT_EQ(t.row_count(), THRESH);

    EXPECT_TRUE(fs::exists(ROOT + "/t/seg_0"));
    EXPECT_TRUE(fs::exists(ROOT + "/t/seg_0/id.col"));
    EXPECT_TRUE(fs::exists(ROOT + "/t/seg_0/meta.bin"));
    EXPECT_TRUE(fs::exists(ROOT + "/t/manifest.bin"));
}

TEST_F(SegmentTest, InsertSpansTwoSegments) {
    auto res = Table::create(ROOT, "t", two_col(), THRESH);
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    for (i32 i = 0; i < static_cast<i32>(THRESH * 2 + 3); ++i)
        ASSERT_TRUE(t.insert(row(i, 0)).is_ok());

    EXPECT_EQ(t.segments().size(), 2u);
    EXPECT_EQ(t.wb_base_row_id(), THRESH * 2);
    EXPECT_EQ(t.row_count(), THRESH * 2 + 3);
}

TEST_F(SegmentTest, ManifestRoundTrip) {
    {
        auto res = Table::create(ROOT, "t", two_col(), THRESH);
        ASSERT_TRUE(res.is_ok());
        auto t = std::move(res.value());
        for (i32 i = 0; i < static_cast<i32>(THRESH + 1); ++i)
            ASSERT_TRUE(t.insert(row(i, i)).is_ok());
        ASSERT_TRUE(t.flush().is_ok());
    }

    auto res = Table::open(ROOT, "t");
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    EXPECT_EQ(t.segments().size(), 1u);
    EXPECT_EQ(t.segments()[0].meta().row_count, THRESH);
    EXPECT_EQ(t.wb_base_row_id(), THRESH);
    EXPECT_EQ(t.row_count(), THRESH + 1);
}

TEST_F(SegmentTest, MarkDeletedInSegment) {
    auto res = Table::create(ROOT, "t", two_col(), THRESH);
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    for (i32 i = 0; i < static_cast<i32>(THRESH + 2); ++i)
        ASSERT_TRUE(t.insert(row(i, 0)).is_ok());

    ASSERT_TRUE(t.mark_deleted({0, THRESH}).is_ok());

    const auto& seg = t.segments()[0];
    EXPECT_TRUE((seg.deleted_bitmap().size() > 0) && (seg.deleted_bitmap()[0] & 0x01u));
    EXPECT_TRUE((t.wb_deleted_ref().size() > 0) && (t.wb_deleted_ref()[0] & 0x01u));
}

TEST_F(SegmentTest, MarkDeletedRoutingAcrossTwoSegments) {
    auto res = Table::create(ROOT, "t", two_col(), THRESH);
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    for (i32 i = 0; i < static_cast<i32>(THRESH * 2 + 1); ++i)
        ASSERT_TRUE(t.insert(row(i, 0)).is_ok());

    ASSERT_TRUE(t.mark_deleted({THRESH - 1, THRESH}).is_ok());

    const auto& seg0 = t.segments()[0];
    const auto& seg1 = t.segments()[1];
    usize byte0 = (THRESH - 1) / 8;
    u8 bit0 = static_cast<u8>(1u << ((THRESH - 1) % 8));
    EXPECT_TRUE(seg0.deleted_bitmap().size() > byte0 && (seg0.deleted_bitmap()[byte0] & bit0));
    EXPECT_TRUE(seg1.deleted_bitmap().size() > 0 && (seg1.deleted_bitmap()[0] & 0x01u));
}

TEST_F(SegmentTest, ConcurrentReadsDontDeadlock) {
    auto res = Table::create(ROOT, "t", two_col(), THRESH);
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());
    for (i32 i = 0; i < static_cast<i32>(THRESH + 2); ++i)
        ASSERT_TRUE(t.insert(row(i, i)).is_ok());

    std::atomic<int> ready{0};
    auto reader = [&] {
        ++ready;
        while (ready.load() < 2) {
        }
        auto lk = t.lock_shared();
        (void)lk;
    };

    std::thread t1(reader);
    std::thread t2(reader);
    t1.join();
    t2.join();
}
