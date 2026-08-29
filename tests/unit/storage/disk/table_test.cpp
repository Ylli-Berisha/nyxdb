#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_table_test";

class TableTest : public ::testing::Test {
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

TEST_F(TableTest, CreateWritesSchemaAndColumnFiles) {
    auto res = Table::create(TEST_ROOT, "users", three_col_schema());
    ASSERT_TRUE(res.is_ok());

    EXPECT_TRUE(fs::exists(fs::path(TEST_ROOT) / "users" / "schema.bin"));
    EXPECT_TRUE(fs::exists(fs::path(TEST_ROOT) / "users" / "id.col"));
    EXPECT_TRUE(fs::exists(fs::path(TEST_ROOT) / "users" / "score.col"));
    EXPECT_TRUE(fs::exists(fs::path(TEST_ROOT) / "users" / "rank.col"));

    auto t = std::move(res.value());
    EXPECT_EQ(t.name(), "users");
    EXPECT_EQ(t.column_count(), 3u);
    EXPECT_EQ(t.row_count(), 0u);
}

TEST_F(TableTest, InsertAndRetrieve) {
    auto res = Table::create(TEST_ROOT, "users", three_col_schema());
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    for (i64 i = 0; i < 100; ++i) {
        std::vector<Value> row = {static_cast<i64>(i), static_cast<f64>(i) * 1.5,
                                  static_cast<i32>(i % 10)};
        auto ins = t.insert(row);
        ASSERT_TRUE(ins.is_ok());
        EXPECT_EQ(ins.value(), static_cast<u64>(i));
    }

    EXPECT_EQ(t.row_count(), 100u);

    for (u64 i = 0; i < 100; ++i) {
        EXPECT_EQ(t.column(0).get_i64(i).value(), static_cast<i64>(i));
        EXPECT_DOUBLE_EQ(t.column(1).get_f64(i).value(), static_cast<f64>(i) * 1.5);
        EXPECT_EQ(t.column(2).get_i32(i).value(), static_cast<i32>(i % 10));
    }
}

TEST_F(TableTest, InsertNullOnlyForNullableColumn) {
    auto res = Table::create(TEST_ROOT, "users", three_col_schema());
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    std::vector<Value> row = {static_cast<i64>(1), std::monostate{}, static_cast<i32>(3)};
    ASSERT_TRUE(t.insert(row).is_ok());
    EXPECT_TRUE(t.column(1).is_null(0));

    std::vector<Value> bad = {std::monostate{}, static_cast<f64>(2.0), static_cast<i32>(3)};
    EXPECT_TRUE(t.insert(bad).is_err()); // id is NOT NULL
}

TEST_F(TableTest, InsertRowSizeMismatchErrors) {
    auto res = Table::create(TEST_ROOT, "users", three_col_schema());
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    std::vector<Value> too_short = {static_cast<i64>(1), static_cast<f64>(2.0)};
    EXPECT_TRUE(t.insert(too_short).is_err());
}

TEST_F(TableTest, InsertTypeMismatchErrors) {
    auto res = Table::create(TEST_ROOT, "users", three_col_schema());
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());

    // schema[0] is INT64 but we pass an i32
    std::vector<Value> wrong = {static_cast<i32>(1), static_cast<f64>(2.0), static_cast<i32>(3)};
    EXPECT_TRUE(t.insert(wrong).is_err());
}

TEST_F(TableTest, CreateFailsOnExistingTable) {
    ASSERT_TRUE(Table::create(TEST_ROOT, "users", three_col_schema()).is_ok());
    auto second = Table::create(TEST_ROOT, "users", three_col_schema());
    EXPECT_TRUE(second.is_err());
}

TEST_F(TableTest, PersistenceAcrossReopens) {
    {
        auto res = Table::create(TEST_ROOT, "users", three_col_schema());
        ASSERT_TRUE(res.is_ok());
        auto t = std::move(res.value());
        for (i64 i = 0; i < 50; ++i) {
            std::vector<Value> row = {static_cast<i64>(i * 2), static_cast<f64>(i) + 0.5,
                                      static_cast<i32>(i)};
            ASSERT_TRUE(t.insert(row).is_ok());
        }
        ASSERT_TRUE(t.flush().is_ok());
        ASSERT_TRUE(t.fsync().is_ok());
    }

    auto res = Table::open(TEST_ROOT, "users");
    ASSERT_TRUE(res.is_ok());
    auto t = std::move(res.value());
    EXPECT_EQ(t.row_count(), 50u);
    EXPECT_EQ(t.column_count(), 3u);
    EXPECT_EQ(t.schema()[0].name, "id");
    EXPECT_EQ(t.schema()[1].name, "score");
    EXPECT_EQ(t.schema()[2].name, "rank");

    for (u64 i = 0; i < 50; ++i) {
        EXPECT_EQ(t.column(0).get_i64(i).value(), static_cast<i64>(i * 2));
        EXPECT_DOUBLE_EQ(t.column(1).get_f64(i).value(), static_cast<f64>(i) + 0.5);
        EXPECT_EQ(t.column(2).get_i32(i).value(), static_cast<i32>(i));
    }
}

TEST_F(TableTest, OpenFailsOnMissingTable) {
    auto res = Table::open(TEST_ROOT, "nonexistent");
    EXPECT_TRUE(res.is_err());
}

TEST_F(TableTest, DuplicateColumnNameRejected) {
    Schema bad = {
        {"id", TypeId::INT64, false},
        {"id", TypeId::INT32, false},
    };
    auto res = Table::create(TEST_ROOT, "bad", bad);
    EXPECT_TRUE(res.is_err());
}
