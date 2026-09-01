#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_table_scan_test";

class TableScanTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    static Schema three_col_schema() {
        return {
            {"id", TypeId::INT64, false},
            {"score", TypeId::DOUBLE, true},
            {"rank", TypeId::INT32, false},
        };
    }
};

TEST_F(TableScanTest, EmptyTableYieldsNothing) {
    auto tres = Table::create(TEST_ROOT, "t", three_col_schema());
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    TableScan scan(&t, {0, 1, 2});
    ASSERT_TRUE(scan.open().is_ok());
    auto n = scan.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(TableScanTest, SingleChunkFitsInOnePage) {
    Schema s = {{"a", TypeId::INT32, false}, {"b", TypeId::INT32, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i32 i = 0; i < 500; ++i)
        ASSERT_TRUE(t.insert({i, i * 2}).is_ok());

    TableScan scan(&t, {0, 1});
    ASSERT_TRUE(scan.open().is_ok());
    auto n = scan.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    EXPECT_EQ(c.row_count(), 500u);
    EXPECT_EQ(c.column_count(), 2u);
    for (size_t i = 0; i < 500; ++i) {
        EXPECT_EQ(c.column(0).get_i32(i), static_cast<i32>(i));
        EXPECT_EQ(c.column(1).get_i32(i), static_cast<i32>(i) * 2);
    }

    auto end = scan.next();
    ASSERT_TRUE(end.is_ok());
    EXPECT_FALSE(end.value().has_value());
}

TEST_F(TableScanTest, ScanAcrossPageBoundary) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 2000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());

    TableScan scan(&t, {0});
    ASSERT_TRUE(scan.open().is_ok());

    std::vector<i64> collected;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            collected.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(collected.size(), 2000u);
    for (size_t i = 0; i < 2000; ++i)
        EXPECT_EQ(collected[i], static_cast<i64>(i));
}

TEST_F(TableScanTest, ScanMultipleChunks) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 5000; ++i)
        ASSERT_TRUE(t.insert({i * 3}).is_ok());

    TableScan scan(&t, {0});
    ASSERT_TRUE(scan.open().is_ok());

    std::vector<size_t> chunk_sizes;
    std::vector<i64> collected;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        chunk_sizes.push_back(c.row_count());
        for (size_t i = 0; i < c.row_count(); ++i)
            collected.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(chunk_sizes.size(), 5u);
    EXPECT_EQ(chunk_sizes[0], 1024u);
    EXPECT_EQ(chunk_sizes[1], 1024u);
    EXPECT_EQ(chunk_sizes[2], 1024u);
    EXPECT_EQ(chunk_sizes[3], 1024u);
    EXPECT_EQ(chunk_sizes[4], 904u);

    ASSERT_EQ(collected.size(), 5000u);
    for (size_t i = 0; i < 5000; ++i)
        EXPECT_EQ(collected[i], static_cast<i64>(i) * 3);
}

TEST_F(TableScanTest, ProjectedSubset) {
    Schema s = {
        {"a", TypeId::INT32, false},
        {"b", TypeId::INT64, false},
        {"c", TypeId::DOUBLE, false},
        {"d", TypeId::INT32, false},
    };
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 100; ++i) {
        std::vector<Value> row = {static_cast<i32>(i), i * 10, static_cast<f64>(i) + 0.5,
                                  static_cast<i32>(-i)};
        ASSERT_TRUE(t.insert(row).is_ok());
    }

    TableScan scan(&t, {0, 2});
    ASSERT_EQ(scan.output_schema().size(), 2u);
    EXPECT_EQ(scan.output_schema()[0].name, "a");
    EXPECT_EQ(scan.output_schema()[1].name, "c");

    ASSERT_TRUE(scan.open().is_ok());
    auto n = scan.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.column_count(), 2u);
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(c.column(0).get_i32(i), static_cast<i32>(i));
        EXPECT_DOUBLE_EQ(c.column(1).get_f64(i), static_cast<f64>(i) + 0.5);
    }
}

TEST_F(TableScanTest, NullsPreservedAcrossChunks) {
    Schema s = {{"x", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i) {
        if (i % 7 == 0)
            ASSERT_TRUE(t.insert({std::monostate{}}).is_ok());
        else
            ASSERT_TRUE(t.insert({i}).is_ok());
    }

    TableScan scan(&t, {0});
    ASSERT_TRUE(scan.open().is_ok());

    size_t seen = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            i64 global = static_cast<i64>(seen + i);
            if (global % 7 == 0) {
                EXPECT_TRUE(c.column(0).is_null(i));
            } else {
                ASSERT_FALSE(c.column(0).is_null(i));
                EXPECT_EQ(c.column(0).get_i64(i), global);
            }
        }
        EXPECT_TRUE(c.column(0).has_nulls());
        seen += c.row_count();
    }
    EXPECT_EQ(seen, 3000u);
}

TEST_F(TableScanTest, MixedTypesRoundTrip) {
    Schema s = {
        {"i32", TypeId::INT32, false},
        {"i64", TypeId::INT64, false},
        {"d", TypeId::DOUBLE, false},
        {"dnul", TypeId::DOUBLE, true},
    };
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 1500; ++i) {
        std::vector<Value> row;
        row.push_back(static_cast<i32>(i));
        row.push_back(i * 100);
        row.push_back(static_cast<f64>(i) + 0.25);
        if (i % 5 == 0)
            row.push_back(std::monostate{});
        else
            row.push_back(static_cast<f64>(i) * 2.0);
        ASSERT_TRUE(t.insert(row).is_ok());
    }

    TableScan scan(&t, {0, 1, 2, 3});
    ASSERT_TRUE(scan.open().is_ok());

    size_t seen = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            i64 g = static_cast<i64>(seen + i);
            EXPECT_EQ(c.column(0).get_i32(i), static_cast<i32>(g));
            EXPECT_EQ(c.column(1).get_i64(i), g * 100);
            EXPECT_DOUBLE_EQ(c.column(2).get_f64(i), static_cast<f64>(g) + 0.25);
            if (g % 5 == 0) {
                EXPECT_TRUE(c.column(3).is_null(i));
            } else {
                ASSERT_FALSE(c.column(3).is_null(i));
                EXPECT_DOUBLE_EQ(c.column(3).get_f64(i), static_cast<f64>(g) * 2.0);
            }
        }
        seen += c.row_count();
    }
    EXPECT_EQ(seen, 1500u);
}

TEST_F(TableScanTest, PersistenceScan) {
    {
        auto tres = Table::create(TEST_ROOT, "t", three_col_schema());
        ASSERT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        for (i64 i = 0; i < 1500; ++i) {
            std::vector<Value> row = {i, static_cast<f64>(i) + 0.5, static_cast<i32>(i % 10)};
            ASSERT_TRUE(t.insert(row).is_ok());
        }
        ASSERT_TRUE(t.flush().is_ok());
        ASSERT_TRUE(t.fsync().is_ok());
    }

    auto tres = Table::open(TEST_ROOT, "t");
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    TableScan scan(&t, {0, 1, 2});
    ASSERT_TRUE(scan.open().is_ok());

    size_t seen = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            i64 g = static_cast<i64>(seen + i);
            EXPECT_EQ(c.column(0).get_i64(i), g);
            EXPECT_DOUBLE_EQ(c.column(1).get_f64(i), static_cast<f64>(g) + 0.5);
            EXPECT_EQ(c.column(2).get_i32(i), static_cast<i32>(g % 10));
        }
        seen += c.row_count();
    }
    EXPECT_EQ(seen, 1500u);
}

TEST_F(TableScanTest, OutOfOrderProjection) {
    auto tres = Table::create(TEST_ROOT, "t", three_col_schema());
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 50; ++i) {
        std::vector<Value> row = {i, static_cast<f64>(i) * 1.5, static_cast<i32>(i)};
        ASSERT_TRUE(t.insert(row).is_ok());
    }

    TableScan scan(&t, {2, 0, 1});
    ASSERT_EQ(scan.output_schema().size(), 3u);
    EXPECT_EQ(scan.output_schema()[0].name, "rank");
    EXPECT_EQ(scan.output_schema()[1].name, "id");
    EXPECT_EQ(scan.output_schema()[2].name, "score");

    ASSERT_TRUE(scan.open().is_ok());
    auto n = scan.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    for (size_t i = 0; i < 50; ++i) {
        EXPECT_EQ(c.column(0).get_i32(i), static_cast<i32>(i));
        EXPECT_EQ(c.column(1).get_i64(i), static_cast<i64>(i));
        EXPECT_DOUBLE_EQ(c.column(2).get_f64(i), static_cast<f64>(i) * 1.5);
    }
}
