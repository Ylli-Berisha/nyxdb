#include "storage/disk/btree_index.h"
#include "storage/disk/index_page.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_btree_index_test";

class BTreeIndexTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
    }
    void TearDown() override { fs::remove_all(ROOT); }
    std::string path(const std::string& n) { return ROOT + "/" + n; }

    static BTreeIndex make_int_index(const std::string& p, bool unique = false) {
        auto r = BTreeIndex::create(p, {{TypeId::INT32, 0}}, {0}, unique);
        EXPECT_TRUE(r.is_ok()) << r.error().message;
        return std::move(r.value());
    }
};

TEST_F(BTreeIndexTest, InsertAndScanAll) {
    auto idx = make_int_index(path("scan.bin"));
    for (int i = 0; i < 10; ++i)
        ASSERT_TRUE(idx.insert({Value{i32(i)}}, static_cast<u64>(i)).is_ok());

    std::vector<u64> found;
    idx.range_scan(nullptr, false, nullptr, false, [&](u64 rid) {
        found.push_back(rid);
        return true;
    });
    EXPECT_EQ(found.size(), 10u);
}

TEST_F(BTreeIndexTest, InsertOutOfOrderScansInOrder) {
    auto idx = make_int_index(path("order.bin"));
    for (int v : {5, 2, 8, 1, 9, 3}) {
        ASSERT_TRUE(idx.insert({Value{i32(v)}}, static_cast<u64>(v)).is_ok());
    }

    std::vector<u64> keys;
    idx.range_scan(nullptr, false, nullptr, false, [&](u64 rid) {
        keys.push_back(rid);
        return true;
    });
    ASSERT_EQ(keys.size(), 6u);
    // row_ids were set equal to the key values, so they should come out sorted
    for (usize i = 1; i < keys.size(); ++i)
        EXPECT_LE(keys[i - 1], keys[i]);
}

TEST_F(BTreeIndexTest, RangeScanWithLoBound) {
    auto idx = make_int_index(path("lo.bin"));
    for (int i = 1; i <= 10; ++i)
        ASSERT_TRUE(idx.insert({Value{i32(i)}}, static_cast<u64>(i)).is_ok());

    std::vector<byte> lo_key(idx.key_size());
    idx.encode_key({Value{i32(5)}}, lo_key.data());

    std::vector<u64> found;
    idx.range_scan(lo_key.data(), true, nullptr, false, [&](u64 rid) {
        found.push_back(rid);
        return true;
    });
    EXPECT_EQ(found.size(), 6u); // 5..10
    EXPECT_EQ(found.front(), 5u);
}

TEST_F(BTreeIndexTest, RangeScanExclusiveLo) {
    auto idx = make_int_index(path("lo_excl.bin"));
    for (int i = 1; i <= 5; ++i)
        ASSERT_TRUE(idx.insert({Value{i32(i)}}, static_cast<u64>(i)).is_ok());

    std::vector<byte> lo_key(idx.key_size());
    idx.encode_key({Value{i32(3)}}, lo_key.data());

    std::vector<u64> found;
    idx.range_scan(lo_key.data(), false, nullptr, false, [&](u64 rid) {
        found.push_back(rid);
        return true;
    });
    EXPECT_EQ(found.size(), 2u); // 4, 5
}

TEST_F(BTreeIndexTest, RangeScanWithHiBound) {
    auto idx = make_int_index(path("hi.bin"));
    for (int i = 1; i <= 10; ++i)
        ASSERT_TRUE(idx.insert({Value{i32(i)}}, static_cast<u64>(i)).is_ok());

    std::vector<byte> hi_key(idx.key_size());
    idx.encode_key({Value{i32(5)}}, hi_key.data());

    std::vector<u64> found;
    idx.range_scan(nullptr, false, hi_key.data(), true, [&](u64 rid) {
        found.push_back(rid);
        return true;
    });
    EXPECT_EQ(found.size(), 5u); // 1..5
}

TEST_F(BTreeIndexTest, RangeScanWithBothBounds) {
    auto idx = make_int_index(path("both.bin"));
    for (int i = 1; i <= 10; ++i)
        ASSERT_TRUE(idx.insert({Value{i32(i)}}, static_cast<u64>(i)).is_ok());

    std::vector<byte> lo_key(idx.key_size()), hi_key(idx.key_size());
    idx.encode_key({Value{i32(3)}}, lo_key.data());
    idx.encode_key({Value{i32(7)}}, hi_key.data());

    std::vector<u64> found;
    idx.range_scan(lo_key.data(), true, hi_key.data(), true, [&](u64 rid) {
        found.push_back(rid);
        return true;
    });
    EXPECT_EQ(found.size(), 5u); // 3,4,5,6,7
}

TEST_F(BTreeIndexTest, EntryCountTracked) {
    auto idx = make_int_index(path("cnt.bin"));
    EXPECT_EQ(idx.entry_count(), 0u);
    ASSERT_TRUE(idx.insert({Value{i32(1)}}, 0).is_ok());
    ASSERT_TRUE(idx.insert({Value{i32(2)}}, 1).is_ok());
    EXPECT_EQ(idx.entry_count(), 2u);
}

TEST_F(BTreeIndexTest, FlushAndReopen) {
    auto p = path("reopen.bin");
    {
        auto idx = make_int_index(p);
        for (int i = 0; i < 5; ++i)
            ASSERT_TRUE(idx.insert({Value{i32(i)}}, static_cast<u64>(i)).is_ok());
        ASSERT_TRUE(idx.flush().is_ok());
    }
    auto r = BTreeIndex::open(p);
    ASSERT_TRUE(r.is_ok());

    std::vector<u64> found;
    r.value().range_scan(nullptr, false, nullptr, false, [&](u64 rid) {
        found.push_back(rid);
        return true;
    });
    EXPECT_EQ(found.size(), 5u);
}

TEST_F(BTreeIndexTest, LargeInsertStaysCorrect) {
    auto idx = make_int_index(path("large.bin"));
    const int N = 500;
    for (int i = N - 1; i >= 0; --i)
        ASSERT_TRUE(idx.insert({Value{i32(i)}}, static_cast<u64>(i)).is_ok());

    EXPECT_EQ(idx.entry_count(), static_cast<u64>(N));

    std::vector<u64> found;
    idx.range_scan(nullptr, false, nullptr, false, [&](u64 rid) {
        found.push_back(rid);
        return true;
    });
    ASSERT_EQ(found.size(), static_cast<usize>(N));
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(found[static_cast<usize>(i)], static_cast<u64>(i));
}
