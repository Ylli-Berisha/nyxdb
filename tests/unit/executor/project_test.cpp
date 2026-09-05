#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/filter.h"
#include "executor/project.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_project_test";

class ProjectTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    Table make_int64_table(const std::string& name, const std::vector<i64>& values,
                           bool nullable = false, std::vector<size_t> null_positions = {}) {
        Schema s = {{"v", TypeId::INT64, nullable}};
        auto tres = Table::create(TEST_ROOT, name, s);
        EXPECT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        std::vector<std::vector<Value>> rows;
        rows.reserve(values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            bool is_null =
                std::find(null_positions.begin(), null_positions.end(), i) != null_positions.end();
            if (is_null)
                rows.push_back({Value{std::monostate{}}});
            else
                rows.push_back({Value{values[i]}});
        }
        EXPECT_TRUE(t.insert_many(rows).is_ok());
        return t;
    }

    Table make_three_col_table(const std::string& name, size_t n) {
        Schema s = {
            {"a", TypeId::INT32, false},
            {"b", TypeId::INT64, false},
            {"c", TypeId::DOUBLE, false},
        };
        auto tres = Table::create(TEST_ROOT, name, s);
        EXPECT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        std::vector<std::vector<Value>> rows;
        rows.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            rows.push_back({Value{static_cast<i32>(i)}, Value{static_cast<i64>(i) * 100},
                            Value{static_cast<f64>(i) + 0.5}});
        }
        EXPECT_TRUE(t.insert_many(rows).is_ok());
        return t;
    }
};

TEST_F(ProjectTest, Identity) {
    auto t = make_three_col_table("t", 50);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1, 2});

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT32), "a", false});
    items.push_back({std::make_unique<ColumnRef>(1, TypeId::INT64), "b", false});
    items.push_back({std::make_unique<ColumnRef>(2, TypeId::DOUBLE), "c", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = p.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        ASSERT_EQ(c.column_count(), 3u);
        for (size_t i = 0; i < c.row_count(); ++i) {
            EXPECT_EQ(c.column(0).get_i32(i), static_cast<i32>(total + i));
            EXPECT_EQ(c.column(1).get_i64(i), static_cast<i64>(total + i) * 100);
            EXPECT_DOUBLE_EQ(c.column(2).get_f64(i), static_cast<f64>(total + i) + 0.5);
        }
        total += c.row_count();
    }
    EXPECT_EQ(total, 50u);
}

TEST_F(ProjectTest, Reorder) {
    auto t = make_three_col_table("t", 20);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1, 2});

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(2, TypeId::DOUBLE), "c", false});
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT32), "a", false});
    items.push_back({std::make_unique<ColumnRef>(1, TypeId::INT64), "b", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    auto n = p.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.column_count(), 3u);
    ASSERT_EQ(c.row_count(), 20u);
    for (size_t i = 0; i < 20; ++i) {
        EXPECT_DOUBLE_EQ(c.column(0).get_f64(i), static_cast<f64>(i) + 0.5);
        EXPECT_EQ(c.column(1).get_i32(i), static_cast<i32>(i));
        EXPECT_EQ(c.column(2).get_i64(i), static_cast<i64>(i) * 100);
    }
}

TEST_F(ProjectTest, DropColumn) {
    auto t = make_three_col_table("t", 10);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1, 2});

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT32), "a", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    auto n = p.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.column_count(), 1u);
    ASSERT_EQ(c.row_count(), 10u);
    for (size_t i = 0; i < 10; ++i)
        EXPECT_EQ(c.column(0).get_i32(i), static_cast<i32>(i));
}

TEST_F(ProjectTest, Computed) {
    std::vector<i64> values;
    for (i64 i = 0; i < 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});

    auto add =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1)}));

    std::vector<ProjectItem> items;
    items.push_back({std::move(add), "v_plus_1", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = p.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(got.size(), 100u);
    for (size_t i = 0; i < 100; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(i) + 1);
}

TEST_F(ProjectTest, LiteralColumn) {
    std::vector<i64> values;
    for (i64 i = 0; i < 30; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<Literal>(Value{static_cast<i64>(42)}), "const_42", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    auto n = p.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.column_count(), 1u);
    ASSERT_EQ(c.row_count(), 30u);
    for (size_t i = 0; i < 30; ++i)
        EXPECT_EQ(c.column(0).get_i64(i), 42);
}

TEST_F(ProjectTest, Mixed) {
    auto t = make_three_col_table("t", 15);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1, 2});

    auto mul =
        std::make_unique<BinaryOp>(BinaryOpKind::MUL, std::make_unique<ColumnRef>(0, TypeId::INT32),
                                   std::make_unique<Literal>(Value{static_cast<i32>(2)}));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(1, TypeId::INT64), "b", false});
    items.push_back({std::move(mul), "a_times_2", false});
    items.push_back({std::make_unique<Literal>(Value{static_cast<i32>(0)}), "zero", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    auto n = p.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.column_count(), 3u);
    ASSERT_EQ(c.row_count(), 15u);
    for (size_t i = 0; i < 15; ++i) {
        EXPECT_EQ(c.column(0).get_i64(i), static_cast<i64>(i) * 100);
        EXPECT_EQ(c.column(1).get_i32(i), static_cast<i32>(i) * 2);
        EXPECT_EQ(c.column(2).get_i32(i), 0);
    }
}

TEST_F(ProjectTest, AcrossChunks) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});

    auto add =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1)}));

    std::vector<ProjectItem> items;
    items.push_back({std::move(add), "v_plus_1", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = p.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        ASSERT_EQ(c.column_count(), 1u);
        for (size_t i = 0; i < c.row_count(); ++i)
            EXPECT_EQ(c.column(0).get_i64(i), static_cast<i64>(total + i) + 1);
        total += c.row_count();
    }
    EXPECT_EQ(total, 3000u);
}

TEST_F(ProjectTest, PreservesRowCount) {
    std::vector<i64> values;
    for (i64 i = 0; i < 1024; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "v", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    auto n = p.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    EXPECT_EQ(n.value()->row_count(), 1024u);
}

TEST_F(ProjectTest, OutputSchema) {
    auto t = make_three_col_table("t", 5);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1, 2});

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(1, TypeId::INT64), "b_alias", false});
    items.push_back({std::make_unique<Literal>(Value{static_cast<i32>(7)}), "seven", true});
    Project p(std::move(scan), std::move(items));

    const Schema& os = p.output_schema();
    ASSERT_EQ(os.size(), 2u);
    EXPECT_EQ(os[0].name, "b_alias");
    EXPECT_EQ(os[0].type, TypeId::INT64);
    EXPECT_FALSE(os[0].nullable);
    EXPECT_EQ(os[1].name, "seven");
    EXPECT_EQ(os[1].type, TypeId::INT32);
    EXPECT_TRUE(os[1].nullable);
}

TEST_F(ProjectTest, EmptyChildYieldsNothing) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "empty", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "v", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    auto n = p.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(ProjectTest, PropagatesExpressionError) {
    std::vector<i64> values;
    for (i64 i = 1; i <= 10; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});

    auto divz =
        std::make_unique<BinaryOp>(BinaryOpKind::DIV, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(0)}));
    std::vector<ProjectItem> items;
    items.push_back({std::move(divz), "bad", false});
    Project p(std::move(scan), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    auto n = p.next();
    EXPECT_TRUE(n.is_err());
}

TEST_F(ProjectTest, MaterializesSelVecInput) {
    std::vector<i64> values;
    for (i64 i = 0; i < 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(50)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred),
                                           FilterStrategy::SELECTION_VECTOR);

    auto add =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1)}));
    std::vector<ProjectItem> items;
    items.push_back({std::move(add), "v_plus_1", false});
    Project p(std::move(filter), std::move(items));
    ASSERT_TRUE(p.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = p.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        EXPECT_FALSE(c.has_sel());
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(got.size(), 50u);
    for (size_t i = 0; i < 50; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(50 + i) + 1);
}
