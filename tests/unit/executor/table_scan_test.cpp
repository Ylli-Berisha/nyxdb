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

TEST_F(TableScanTest, ScanRangeFullOverlap) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());

    TableScan scan(&t, {0},
                   ScanRange{0, Value{static_cast<i64>(500)}, Value{static_cast<i64>(2500)}});
    ASSERT_TRUE(scan.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        total += n.value()->row_count();
    }
    EXPECT_EQ(total, 3000u);
}

TEST_F(TableScanTest, ScanRangeSkipsTailPage) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());
    u16 cap = t.column(0).page_capacity();

    TableScan scan(&t, {0},
                   ScanRange{0, Value{static_cast<i64>(0)}, Value{static_cast<i64>(1500)}});
    ASSERT_TRUE(scan.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    EXPECT_EQ(got.size(), static_cast<size_t>(cap) * 2u);
    for (size_t i = 0; i < got.size(); ++i)
        EXPECT_EQ(got[i], static_cast<i64>(i));
}

TEST_F(TableScanTest, ScanRangeSkipsHeadPage) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());
    u16 cap = t.column(0).page_capacity();

    TableScan scan(&t, {0},
                   ScanRange{0, Value{static_cast<i64>(1500)}, Value{static_cast<i64>(3000)}});
    ASSERT_TRUE(scan.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    size_t expected = 3000u - static_cast<size_t>(cap);
    EXPECT_EQ(got.size(), expected);
    for (size_t i = 0; i < got.size(); ++i)
        EXPECT_EQ(got[i], static_cast<i64>(i) + static_cast<i64>(cap));
}

TEST_F(TableScanTest, ScanRangeNoOverlap) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 1000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());

    TableScan scan(&t, {0},
                   ScanRange{0, Value{static_cast<i64>(10000)}, Value{static_cast<i64>(20000)}});
    ASSERT_TRUE(scan.open().is_ok());

    auto n = scan.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(TableScanTest, ScanRangeLowerUnbounded) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());
    u16 cap = t.column(0).page_capacity();

    TableScan scan(&t, {0}, ScanRange{0, std::nullopt, Value{static_cast<i64>(1500)}});
    ASSERT_TRUE(scan.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        total += n.value()->row_count();
    }
    EXPECT_EQ(total, static_cast<size_t>(cap) * 2u);
}

TEST_F(TableScanTest, ScanRangeUpperUnbounded) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());
    u16 cap = t.column(0).page_capacity();

    TableScan scan(&t, {0}, ScanRange{0, Value{static_cast<i64>(1500)}, std::nullopt});
    ASSERT_TRUE(scan.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        total += n.value()->row_count();
    }
    EXPECT_EQ(total, 3000u - static_cast<size_t>(cap));
}

TEST_F(TableScanTest, ScanRangeSurvivorMerging) {
    // All three pages survive → survivors are merged into one range → chunks are 1024-aligned,
    // not page-fragmented.
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i)
        ASSERT_TRUE(t.insert({i}).is_ok());

    TableScan scan(&t, {0},
                   ScanRange{0, Value{static_cast<i64>(0)}, Value{static_cast<i64>(3000)}});
    ASSERT_TRUE(scan.open().is_ok());

    std::vector<size_t> chunk_sizes;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        chunk_sizes.push_back(n.value()->row_count());
    }
    ASSERT_EQ(chunk_sizes.size(), 3u);
    EXPECT_EQ(chunk_sizes[0], 1024u);
    EXPECT_EQ(chunk_sizes[1], 1024u);
    EXPECT_EQ(chunk_sizes[2], 952u);
}

TEST_F(TableScanTest, ScanRangeConstrainedColNotInProjected) {
    Schema s = {{"a", TypeId::INT64, false}, {"b", TypeId::INT32, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < 3000; ++i)
        ASSERT_TRUE(t.insert({i, static_cast<i32>(i)}).is_ok());
    u16 cap_b = t.column(1).page_capacity();

    // Project only col 0; zone-check on col 1 (INT32).
    TableScan scan(&t, {0}, ScanRange{1, std::nullopt, Value{static_cast<i32>(1500)}});
    ASSERT_TRUE(scan.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        ASSERT_EQ(c.column_count(), 1u);
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    // We should emit exactly those rows whose page (in col b) has max <= 1500. INT32 packs
    // more rows per page than INT64, so page count differs — but the rule is the same:
    // include pages where max_i32 >= r.lo (unbounded) AND min_i32 <= 1500. Every page whose
    // min > 1500 is skipped; only the last page(s) get dropped.
    ASSERT_GT(got.size(), 0u);
    for (size_t i = 0; i < got.size(); ++i)
        EXPECT_EQ(got[i], static_cast<i64>(i));
    // Row count is a multiple of cap_b (whole pages of col b survive).
    EXPECT_EQ(got.size() % static_cast<size_t>(cap_b), 0u);
}

TEST_F(TableScanTest, ScanRangeEmptyTable) {
    Schema s = {{"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    TableScan scan(&t, {0}, ScanRange{0, Value{static_cast<i64>(0)}, Value{static_cast<i64>(100)}});
    ASSERT_TRUE(scan.open().is_ok());
    auto n = scan.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(TableScanTest, ScanRangeNullableColumnMixedNulls) {
    // Build a table where page 0 has non-null values in-range, page 1 is all null,
    // page 2 has non-null values in-range but wholly outside the predicate range.
    Schema s = {{"x", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "t", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    // Discover page capacity for nullable INT64.
    // Insert one value, get capacity, remove not applicable — we can only read cap after any
    // insert.
    ASSERT_TRUE(t.insert({static_cast<i64>(0)}).is_ok());
    u16 cap = t.column(0).page_capacity();

    // Fill remainder of page 0 with in-range non-null values.
    for (u16 i = 1; i < cap; ++i)
        ASSERT_TRUE(t.insert({static_cast<i64>(i)}).is_ok());
    // Fill page 1 entirely with nulls.
    for (u16 i = 0; i < cap; ++i)
        ASSERT_TRUE(t.insert({std::monostate{}}).is_ok());
    // Fill page 2 with non-null values far out of range.
    for (u16 i = 0; i < cap; ++i)
        ASSERT_TRUE(t.insert({static_cast<i64>(1000000 + i)}).is_ok());

    // Predicate: values <= cap-1 (i.e., page 0's max).
    // Page 0: pmin=0, pmax=cap-1, r.hi=cap-1 → include.
    // Page 1: pmin/pmax = nullopt (all null) → skip.
    // Page 2: pmin=1000000, r.hi=cap-1 → skip (1000000 > cap-1).
    TableScan scan(&t, {0}, ScanRange{0, std::nullopt, Value{static_cast<i64>(cap - 1)}});
    ASSERT_TRUE(scan.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        total += n.value()->row_count();
    }
    EXPECT_EQ(total, static_cast<size_t>(cap));
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
