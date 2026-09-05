#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/sort.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_sort_test";

class SortTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    Table make_int64_table(const std::string& name, const std::vector<i64>& values) {
        Schema s = {{"v", TypeId::INT64, false}};
        auto tres = Table::create(TEST_ROOT, name, s);
        EXPECT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        std::vector<std::vector<Value>> rows;
        rows.reserve(values.size());
        for (i64 v : values)
            rows.push_back({Value{v}});
        EXPECT_TRUE(t.insert_many(rows).is_ok());
        return t;
    }

    static std::vector<SortKey> key_asc_col0_i64() {
        std::vector<SortKey> keys;
        keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT64), SortDirection::ASC,
                               NullOrder::LAST});
        return keys;
    }

    static std::vector<i64> drain_i64(Sort& s) {
        std::vector<i64> out;
        while (true) {
            auto n = s.next();
            EXPECT_TRUE(n.is_ok());
            if (!n.value().has_value())
                break;
            const Chunk& c = *n.value();
            EXPECT_FALSE(c.has_sel());
            for (size_t i = 0; i < c.row_count(); ++i)
                out.push_back(c.column(0).get_i64(i));
        }
        return out;
    }
};

TEST_F(SortTest, SingleChunkAscending) {
    std::vector<i64> input = {5, 1, 4, 2, 3, 9, 7, 8, 6, 0};
    auto t = make_int64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), key_asc_col0_i64());
    ASSERT_TRUE(sort.open().is_ok());

    auto got = drain_i64(sort);
    std::vector<i64> expected = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    EXPECT_EQ(got, expected);
}

TEST_F(SortTest, AcrossManyInputChunks) {
    std::vector<i64> input;
    input.reserve(3000);
    for (i64 i = 0; i < 3000; ++i)
        input.push_back(2999 - i);
    auto t = make_int64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), key_asc_col0_i64());
    ASSERT_TRUE(sort.open().is_ok());

    auto got = drain_i64(sort);
    ASSERT_EQ(got.size(), 3000u);
    for (size_t i = 0; i < 3000; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(i));
}

TEST_F(SortTest, EmitsChunkSizedSlices) {
    std::vector<i64> input;
    input.reserve(2500);
    for (i64 i = 0; i < 2500; ++i)
        input.push_back(i);
    auto t = make_int64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), key_asc_col0_i64());
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<size_t> chunk_sizes;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        chunk_sizes.push_back(n.value()->row_count());
    }
    ASSERT_EQ(chunk_sizes.size(), 3u);
    EXPECT_EQ(chunk_sizes[0], TableScan::CHUNK_SIZE);
    EXPECT_EQ(chunk_sizes[1], TableScan::CHUNK_SIZE);
    EXPECT_EQ(chunk_sizes[2], 2500u - 2 * TableScan::CHUNK_SIZE);
}

TEST_F(SortTest, EmptyChildYieldsNothing) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "empty", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), key_asc_col0_i64());
    ASSERT_TRUE(sort.open().is_ok());

    auto n = sort.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(SortTest, SingleRow) {
    auto t = make_int64_table("t", {42});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), key_asc_col0_i64());
    ASSERT_TRUE(sort.open().is_ok());

    auto got = drain_i64(sort);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], 42);
}

TEST_F(SortTest, PayloadPreservedAlongsideKey) {
    Schema s = {{"k", TypeId::INT64, false}, {"p", TypeId::INT32, false}};
    auto tres = Table::create(TEST_ROOT, "kv", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i64>(3)}, Value{static_cast<i32>(30)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i32>(10)}},
        {Value{static_cast<i64>(4)}, Value{static_cast<i32>(40)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i32>(11)}},
        {Value{static_cast<i64>(5)}, Value{static_cast<i32>(50)}},
        {Value{static_cast<i64>(9)}, Value{static_cast<i32>(90)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i32>(20)}},
        {Value{static_cast<i64>(6)}, Value{static_cast<i32>(60)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    Sort sort(std::move(scan), key_asc_col0_i64());
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<std::pair<i64, i32>> got;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i32(i));
    }
    std::vector<std::pair<i64, i32>> expected = {
        {1, 10}, {1, 11}, {2, 20}, {3, 30}, {4, 40}, {5, 50}, {6, 60}, {9, 90},
    };
    EXPECT_EQ(got, expected);
}
