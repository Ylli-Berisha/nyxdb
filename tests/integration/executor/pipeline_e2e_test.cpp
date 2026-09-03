#include "executor/chunk.h"
#include "executor/expression.h"
#include "executor/filter.h"
#include "executor/limit.h"
#include "executor/operator.h"
#include "executor/project.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <utility>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_pipeline_e2e";

class PipelineE2ETest : public ::testing::Test {
  protected:
    static constexpr i64 N = 10000;

    void SetUp() override {
        fs::remove_all(TEST_ROOT);
        auto tres = Table::create(TEST_ROOT, "t", wide_schema());
        ASSERT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        std::vector<std::vector<Value>> bulk;
        bulk.reserve(N);
        for (i64 i = 0; i < N; ++i) {
            bulk.push_back({
                Value{i},
                Value{static_cast<i32>(i % 100)},
                Value{static_cast<f64>(i) * 1.5 + 0.25},
                weight_is_null(i) ? Value{std::monostate{}} : Value{expected_weight(i)},
            });
        }
        ASSERT_TRUE(t.insert_many(bulk).is_ok());
        ASSERT_TRUE(t.flush().is_ok());
        ASSERT_TRUE(t.fsync().is_ok());
    }

    void TearDown() override { fs::remove_all(TEST_ROOT); }

    static Schema wide_schema() {
        return {
            {"id", TypeId::INT64, false},
            {"cat", TypeId::INT32, false},
            {"score", TypeId::DOUBLE, false},
            {"weight", TypeId::DOUBLE, true},
        };
    }

    static bool weight_is_null(i64 i) { return i % 11 == 0; }
    static f64 expected_weight(i64 i) { return static_cast<f64>(i) / 3.0; }

    Table open_table() {
        auto tres = Table::open(TEST_ROOT, "t");
        EXPECT_TRUE(tres.is_ok());
        return std::move(tres.value());
    }

    static std::vector<i64> drain_i64(Operator& op, size_t col) {
        EXPECT_TRUE(op.open().is_ok());
        std::vector<i64> out;
        while (true) {
            auto n = op.next();
            EXPECT_TRUE(n.is_ok());
            if (!n.value().has_value())
                break;
            const Chunk& c = *n.value();
            for (size_t i = 0; i < c.row_count(); ++i)
                out.push_back(c.column(col).get_i64(i));
        }
        op.close();
        return out;
    }

    static std::vector<std::pair<i64, i64>> drain_i64_pair(Operator& op, size_t c0, size_t c1) {
        EXPECT_TRUE(op.open().is_ok());
        std::vector<std::pair<i64, i64>> out;
        while (true) {
            auto n = op.next();
            EXPECT_TRUE(n.is_ok());
            if (!n.value().has_value())
                break;
            const Chunk& c = *n.value();
            for (size_t i = 0; i < c.row_count(); ++i)
                out.emplace_back(c.column(c0).get_i64(i), c.column(c1).get_i64(i));
        }
        op.close();
        return out;
    }
};

TEST_F(PipelineE2ETest, ScanFilterProjectLimit) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(5000)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    auto add =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1)}));
    items.push_back({std::move(add), "id_plus_1", false});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 100);

    auto pairs = drain_i64_pair(limit, 0, 1);
    ASSERT_EQ(pairs.size(), 100u);
    EXPECT_EQ(pairs.front(), (std::pair<i64, i64>{5000, 5001}));
    EXPECT_EQ(pairs.back(), (std::pair<i64, i64>{5099, 5100}));
    for (size_t i = 0; i < pairs.size(); ++i)
        EXPECT_EQ(pairs[i].second, pairs[i].first + 1);
}

TEST_F(PipelineE2ETest, ScanFilterProjectLimitOffset) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(5000)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    auto add =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1)}));
    items.push_back({std::move(add), "id_plus_1", false});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 50, 100);

    auto pairs = drain_i64_pair(limit, 0, 1);
    ASSERT_EQ(pairs.size(), 100u);
    EXPECT_EQ(pairs.front(), (std::pair<i64, i64>{5050, 5051}));
    EXPECT_EQ(pairs.back(), (std::pair<i64, i64>{5149, 5150}));
}

TEST_F(PipelineE2ETest, FilterDropsAll) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(100000)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 100);

    auto out = drain_i64(limit, 0);
    EXPECT_TRUE(out.empty());
}

TEST_F(PipelineE2ETest, LimitZero) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(0)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 0);

    auto out = drain_i64(limit, 0);
    EXPECT_TRUE(out.empty());
}

TEST_F(PipelineE2ETest, OffsetLargerThanFilteredRows) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::EQ, std::make_unique<ColumnRef>(1, TypeId::INT32),
                                   std::make_unique<Literal>(Value{static_cast<i32>(42)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 5000, 10);

    auto out = drain_i64(limit, 0);
    EXPECT_TRUE(out.empty());
}

TEST_F(PipelineE2ETest, NullsInFilterPredicate) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{3});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::DOUBLE),
                                   std::make_unique<Literal>(Value{5.0}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::DOUBLE), "weight", true});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 50);

    ASSERT_TRUE(limit.open().is_ok());
    std::vector<f64> got;
    while (true) {
        auto n = limit.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            ASSERT_FALSE(c.column(0).is_null(i));
            got.push_back(c.column(0).get_f64(i));
        }
    }
    limit.close();
    ASSERT_EQ(got.size(), 50u);
    for (f64 v : got)
        EXPECT_GT(v, 5.0);
}

TEST_F(PipelineE2ETest, FilterProtectsProject) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT32),
                                   std::make_unique<Literal>(Value{static_cast<i32>(0)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    auto div = std::make_unique<BinaryOp>(BinaryOpKind::DIV,
                                          std::make_unique<Literal>(Value{static_cast<i32>(100)}),
                                          std::make_unique<ColumnRef>(0, TypeId::INT32));
    std::vector<ProjectItem> items;
    items.push_back({std::move(div), "hundred_over_cat", false});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 50);

    ASSERT_TRUE(limit.open().is_ok());
    size_t rows = 0;
    while (true) {
        auto n = limit.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            i32 v = c.column(0).get_i32(i);
            EXPECT_GE(v, 1);
            EXPECT_LE(v, 100);
        }
        rows += c.row_count();
    }
    limit.close();
    EXPECT_EQ(rows, 50u);
}

TEST_F(PipelineE2ETest, ScanRangeMatchesFilterResult) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(
        &t, std::vector<size_t>{0}, ScanRange{0, Value{static_cast<i64>(5000)}, std::nullopt});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(5000)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    auto add =
        std::make_unique<BinaryOp>(BinaryOpKind::ADD, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1)}));
    items.push_back({std::move(add), "id_plus_1", false});
    auto project = std::make_unique<Project>(std::move(filter), std::move(items));

    Limit limit(std::move(project), 100);

    auto pairs = drain_i64_pair(limit, 0, 1);
    ASSERT_EQ(pairs.size(), 100u);
    EXPECT_EQ(pairs.front(), (std::pair<i64, i64>{5000, 5001}));
    EXPECT_EQ(pairs.back(), (std::pair<i64, i64>{5099, 5100}));
}

TEST_F(PipelineE2ETest, ScanRangeNarrowsIO) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(
        &t, std::vector<size_t>{0},
        ScanRange{0, Value{static_cast<i64>(5000)}, Value{static_cast<i64>(5100)}});
    auto pred_lo =
        std::make_unique<BinaryOp>(BinaryOpKind::GE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(5000)}));
    auto pred_hi =
        std::make_unique<BinaryOp>(BinaryOpKind::LE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(5100)}));
    auto filter_lo = std::make_unique<Filter>(std::move(scan), std::move(pred_lo));
    auto filter_hi = std::make_unique<Filter>(std::move(filter_lo), std::move(pred_hi));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    Project project(std::move(filter_hi), std::move(items));

    auto out = drain_i64(project, 0);
    ASSERT_EQ(out.size(), 101u);
    for (size_t i = 0; i < 101; ++i)
        EXPECT_EQ(out[i], static_cast<i64>(5000 + i));
}

TEST_F(PipelineE2ETest, FullDrainNoLimit) {
    auto t = open_table();

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::LT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(5000)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred));

    std::vector<ProjectItem> items;
    items.push_back({std::make_unique<ColumnRef>(0, TypeId::INT64), "id", false});
    Project project(std::move(filter), std::move(items));

    auto out = drain_i64(project, 0);
    ASSERT_EQ(out.size(), 5000u);
    for (size_t i = 0; i < 5000; ++i)
        EXPECT_EQ(out[i], static_cast<i64>(i));
}
