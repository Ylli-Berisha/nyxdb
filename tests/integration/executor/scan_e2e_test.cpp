#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_scan_e2e_test";

class ScanE2ETest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    static Schema wide_schema() {
        return {
            {"id", TypeId::INT64, false},
            {"count", TypeId::INT32, false},
            {"score", TypeId::DOUBLE, false},
            {"weight", TypeId::DOUBLE, true},
        };
    }

    static Value expected_id(i64 g) { return g; }
    static Value expected_count(i64 g) { return static_cast<i32>(g % 100); }
    static Value expected_score(i64 g) { return static_cast<f64>(g) * 1.5 + 0.25; }
    static bool weight_is_null(i64 g) { return g % 11 == 0; }
    static f64 expected_weight_value(i64 g) { return static_cast<f64>(g) / 3.0; }
};

TEST_F(ScanE2ETest, FullScanAllTypes) {
    constexpr i64 N = 10000;
    {
        auto tres = Table::create(TEST_ROOT, "t", wide_schema());
        ASSERT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());

        for (i64 i = 0; i < 3000; ++i) {
            std::vector<Value> row = {expected_id(i), expected_count(i), expected_score(i),
                                      weight_is_null(i) ? Value{std::monostate{}}
                                                        : Value{expected_weight_value(i)}};
            ASSERT_TRUE(t.insert(row).is_ok());
        }

        std::vector<std::vector<Value>> bulk;
        bulk.reserve(N - 3000);
        for (i64 i = 3000; i < N; ++i) {
            bulk.push_back(
                {expected_id(i), expected_count(i), expected_score(i),
                 weight_is_null(i) ? Value{std::monostate{}} : Value{expected_weight_value(i)}});
        }
        ASSERT_TRUE(t.insert_many(bulk).is_ok());
        ASSERT_TRUE(t.flush().is_ok());
        ASSERT_TRUE(t.fsync().is_ok());
    }

    auto tres = Table::open(TEST_ROOT, "t");
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    EXPECT_EQ(t.row_count(), static_cast<u64>(N));

    TableScan scan(&t, {0, 1, 2, 3});
    ASSERT_TRUE(scan.open().is_ok());

    std::vector<size_t> chunk_sizes;
    i64 seen = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        chunk_sizes.push_back(c.row_count());
        for (size_t i = 0; i < c.row_count(); ++i) {
            i64 g = seen + static_cast<i64>(i);
            EXPECT_EQ(c.column(0).get_i64(i), std::get<i64>(expected_id(g)));
            EXPECT_EQ(c.column(1).get_i32(i), std::get<i32>(expected_count(g)));
            EXPECT_DOUBLE_EQ(c.column(2).get_f64(i), std::get<f64>(expected_score(g)));
            if (weight_is_null(g)) {
                EXPECT_TRUE(c.column(3).is_null(i));
            } else {
                ASSERT_FALSE(c.column(3).is_null(i));
                EXPECT_DOUBLE_EQ(c.column(3).get_f64(i), expected_weight_value(g));
            }
        }
        seen += static_cast<i64>(c.row_count());
    }
    EXPECT_EQ(seen, N);

    ASSERT_EQ(chunk_sizes.size(), 10u);
    for (size_t i = 0; i < 9; ++i)
        EXPECT_EQ(chunk_sizes[i], 1024u);
    EXPECT_EQ(chunk_sizes[9], 784u);
}

TEST_F(ScanE2ETest, ProjectedScan) {
    constexpr i64 N = 3500;
    auto tres = Table::create(TEST_ROOT, "t", wide_schema());
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    for (i64 i = 0; i < N; ++i) {
        std::vector<Value> row = {expected_id(i), expected_count(i), expected_score(i),
                                  weight_is_null(i) ? Value{std::monostate{}}
                                                    : Value{expected_weight_value(i)}};
        ASSERT_TRUE(t.insert(row).is_ok());
    }

    TableScan scan(&t, {0, 3});
    ASSERT_EQ(scan.output_schema().size(), 2u);
    EXPECT_EQ(scan.output_schema()[0].name, "id");
    EXPECT_EQ(scan.output_schema()[1].name, "weight");

    ASSERT_TRUE(scan.open().is_ok());
    i64 seen = 0;
    size_t chunk_count = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        ++chunk_count;
        ASSERT_EQ(c.column_count(), 2u);
        for (size_t i = 0; i < c.row_count(); ++i) {
            i64 g = seen + static_cast<i64>(i);
            EXPECT_EQ(c.column(0).get_i64(i), g);
            if (weight_is_null(g)) {
                EXPECT_TRUE(c.column(1).is_null(i));
            } else {
                ASSERT_FALSE(c.column(1).is_null(i));
                EXPECT_DOUBLE_EQ(c.column(1).get_f64(i), expected_weight_value(g));
            }
        }
        seen += static_cast<i64>(c.row_count());
    }
    EXPECT_EQ(seen, N);
    EXPECT_EQ(chunk_count, 4u);
}

TEST_F(ScanE2ETest, ScanAfterInterleavedInserts) {
    constexpr i64 N = 5000;
    auto tres = Table::create(TEST_ROOT, "t", wide_schema());
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    i64 cursor = 0;
    while (cursor < N) {
        for (i64 i = 0; i < 100 && cursor < N; ++i, ++cursor) {
            std::vector<Value> row = {
                expected_id(cursor), expected_count(cursor), expected_score(cursor),
                weight_is_null(cursor) ? Value{std::monostate{}}
                                       : Value{expected_weight_value(cursor)}};
            ASSERT_TRUE(t.insert(row).is_ok());
        }
        std::vector<std::vector<Value>> bulk;
        for (i64 i = 0; i < 100 && cursor < N; ++i, ++cursor) {
            bulk.push_back({expected_id(cursor), expected_count(cursor), expected_score(cursor),
                            weight_is_null(cursor) ? Value{std::monostate{}}
                                                   : Value{expected_weight_value(cursor)}});
        }
        if (!bulk.empty()) {
            ASSERT_TRUE(t.insert_many(bulk).is_ok());
        }
    }
    EXPECT_EQ(t.row_count(), static_cast<u64>(N));

    TableScan scan(&t, {0, 1, 2, 3});
    ASSERT_TRUE(scan.open().is_ok());
    i64 seen = 0;
    while (true) {
        auto n = scan.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            i64 g = seen + static_cast<i64>(i);
            EXPECT_EQ(c.column(0).get_i64(i), g);
            EXPECT_EQ(c.column(1).get_i32(i), static_cast<i32>(g % 100));
            EXPECT_DOUBLE_EQ(c.column(2).get_f64(i), static_cast<f64>(g) * 1.5 + 0.25);
            if (weight_is_null(g)) {
                EXPECT_TRUE(c.column(3).is_null(i));
            } else {
                ASSERT_FALSE(c.column(3).is_null(i));
                EXPECT_DOUBLE_EQ(c.column(3).get_f64(i), expected_weight_value(g));
            }
        }
        seen += static_cast<i64>(c.row_count());
    }
    EXPECT_EQ(seen, N);
}
