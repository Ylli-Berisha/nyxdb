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

TEST_F(SortTest, DescendingSingleKey) {
    std::vector<i64> input = {3, 1, 4, 1, 5, 9, 2, 6};
    auto t = make_int64_table("t", input);

    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT64), SortDirection::DESC,
                           NullOrder::LAST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), std::move(keys));
    ASSERT_TRUE(sort.open().is_ok());

    auto got = drain_i64(sort);
    std::vector<i64> expected = {9, 6, 5, 4, 3, 2, 1, 1};
    EXPECT_EQ(got, expected);
}

TEST_F(SortTest, MultiKeyTiebreak) {
    Schema s = {{"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "mk", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(20)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(15)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(10)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(5)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT64), SortDirection::ASC,
                           NullOrder::LAST});
    keys.push_back(SortKey{std::make_unique<ColumnRef>(1, TypeId::INT64), SortDirection::ASC,
                           NullOrder::LAST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    Sort sort(std::move(scan), std::move(keys));
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<std::pair<i64, i64>> got;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i));
    }
    std::vector<std::pair<i64, i64>> expected = {{1, 5}, {1, 15}, {2, 10}, {2, 20}};
    EXPECT_EQ(got, expected);
}

TEST_F(SortTest, MultiKeyMixedDirections) {
    Schema s = {{"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "mkd", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(10)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(20)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(10)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(20)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT64), SortDirection::ASC,
                           NullOrder::LAST});
    keys.push_back(SortKey{std::make_unique<ColumnRef>(1, TypeId::INT64), SortDirection::DESC,
                           NullOrder::LAST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    Sort sort(std::move(scan), std::move(keys));
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<std::pair<i64, i64>> got;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i));
    }
    std::vector<std::pair<i64, i64>> expected = {{1, 20}, {1, 10}, {2, 20}, {2, 10}};
    EXPECT_EQ(got, expected);
}

TEST_F(SortTest, NullsLast) {
    Schema s = {{"v", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "nulls_last", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i64>(3)}}, {Value{std::monostate{}}},    {Value{static_cast<i64>(1)}},
        {Value{std::monostate{}}},    {Value{static_cast<i64>(2)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT64), SortDirection::ASC,
                           NullOrder::LAST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), std::move(keys));
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<std::pair<bool, i64>> got;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            bool is_null = c.column(0).is_null(i);
            got.emplace_back(is_null, is_null ? 0 : c.column(0).get_i64(i));
        }
    }
    std::vector<std::pair<bool, i64>> expected = {
        {false, 1}, {false, 2}, {false, 3}, {true, 0}, {true, 0},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(SortTest, NullsFirst) {
    Schema s = {{"v", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "nulls_first", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i64>(3)}}, {Value{std::monostate{}}},    {Value{static_cast<i64>(1)}},
        {Value{std::monostate{}}},    {Value{static_cast<i64>(2)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT64), SortDirection::ASC,
                           NullOrder::FIRST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    Sort sort(std::move(scan), std::move(keys));
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<std::pair<bool, i64>> got;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            bool is_null = c.column(0).is_null(i);
            got.emplace_back(is_null, is_null ? 0 : c.column(0).get_i64(i));
        }
    }
    std::vector<std::pair<bool, i64>> expected = {
        {true, 0}, {true, 0}, {false, 1}, {false, 2}, {false, 3},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(SortTest, MixedKeyTypes) {
    Schema s = {{"a", TypeId::INT32, false}, {"b", TypeId::DOUBLE, false}};
    auto tres = Table::create(TEST_ROOT, "mkt", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i32>(1)}, Value{static_cast<f64>(2.5)}},
        {Value{static_cast<i32>(1)}, Value{static_cast<f64>(1.5)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<f64>(0.5)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT32), SortDirection::ASC,
                           NullOrder::LAST});
    keys.push_back(SortKey{std::make_unique<ColumnRef>(1, TypeId::DOUBLE), SortDirection::ASC,
                           NullOrder::LAST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    Sort sort(std::move(scan), std::move(keys));
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<std::pair<i32, f64>> got;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i32(i), c.column(1).get_f64(i));
    }
    std::vector<std::pair<i32, f64>> expected = {{1, 1.5}, {1, 2.5}, {2, 0.5}};
    EXPECT_EQ(got, expected);
}

TEST_F(SortTest, StablePreservesOriginalOrder) {
    Schema s = {{"k", TypeId::INT64, false}, {"p", TypeId::INT32, false}};
    auto tres = Table::create(TEST_ROOT, "stable", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (i32 i = 0; i < 100; ++i) {
        i64 k = i % 5;
        rows.push_back({Value{k}, Value{i}});
    }
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::make_unique<ColumnRef>(0, TypeId::INT64), SortDirection::ASC,
                           NullOrder::LAST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    Sort sort(std::move(scan), std::move(keys));
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
    ASSERT_EQ(got.size(), 100u);
    for (size_t i = 0; i < 100; ++i) {
        i64 expected_k = static_cast<i64>(i / 20);
        i32 expected_p = static_cast<i32>((i % 20) * 5 + expected_k);
        EXPECT_EQ(got[i].first, expected_k) << " at i=" << i;
        EXPECT_EQ(got[i].second, expected_p) << " at i=" << i;
    }
}

TEST_F(SortTest, ExpressionKey) {
    Schema s = {{"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "expr", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(9)}},
        {Value{static_cast<i64>(5)}, Value{static_cast<i64>(3)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(2)}},
        {Value{static_cast<i64>(3)}, Value{static_cast<i64>(3)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto sum =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(1, TypeId::INT64));
    std::vector<SortKey> keys;
    keys.push_back(SortKey{std::move(sum), SortDirection::ASC, NullOrder::LAST});

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    Sort sort(std::move(scan), std::move(keys));
    ASSERT_TRUE(sort.open().is_ok());

    std::vector<std::pair<i64, i64>> got;
    while (true) {
        auto n = sort.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i));
    }
    std::vector<std::pair<i64, i64>> expected = {{2, 2}, {3, 3}, {5, 3}, {1, 9}};
    EXPECT_EQ(got, expected);
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
