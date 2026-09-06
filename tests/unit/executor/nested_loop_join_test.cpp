#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/nested_loop_join.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <tuple>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_nl_join_test";

using Row4 = std::tuple<i64, i64, i64, i64>;

class NestedLoopJoinTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    Table make_kv_table(const std::string& name, const std::vector<std::pair<i64, i64>>& rows) {
        Schema s = {{"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}};
        auto tres = Table::create(TEST_ROOT, name, s);
        EXPECT_TRUE(tres.is_ok());
        auto t = std::move(tres.value());
        std::vector<std::vector<Value>> data;
        data.reserve(rows.size());
        for (const auto& r : rows)
            data.push_back({Value{r.first}, Value{r.second}});
        EXPECT_TRUE(t.insert_many(data).is_ok());
        return t;
    }

    static std::vector<Row4> drain(Operator& op) {
        EXPECT_TRUE(op.open().is_ok());
        std::vector<Row4> out;
        while (true) {
            auto n = op.next();
            EXPECT_TRUE(n.is_ok());
            if (!n.value().has_value())
                break;
            const Chunk& c = *n.value();
            EXPECT_FALSE(c.has_sel());
            for (size_t i = 0; i < c.row_count(); ++i)
                out.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i),
                                 c.column(2).get_i64(i), c.column(3).get_i64(i));
        }
        op.close();
        return out;
    }

    static std::vector<Row4> sorted(std::vector<Row4> v) {
        std::sort(v.begin(), v.end());
        return v;
    }
};

TEST_F(NestedLoopJoinTest, EquiJoinViaNL) {
    auto l = make_kv_table("l", {{1, 10}, {2, 20}, {3, 30}});
    auto r = make_kv_table("r", {{1, 100}, {2, 200}, {3, 300}});

    auto outer_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    auto inner_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::EQ, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(2, TypeId::INT64));
    NestedLoopJoin join(std::move(outer_scan), std::move(inner_scan), std::move(pred));

    auto got = sorted(drain(join));
    std::vector<Row4> expected = {{1, 10, 1, 100}, {2, 20, 2, 200}, {3, 30, 3, 300}};
    EXPECT_EQ(got, expected);
}

TEST_F(NestedLoopJoinTest, ThetaJoinLessThan) {
    auto l = make_kv_table("l", {{1, 0}, {2, 0}, {3, 0}});
    auto r = make_kv_table("r", {{2, 0}, {4, 0}});

    auto outer_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    auto inner_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::LT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(2, TypeId::INT64));
    NestedLoopJoin join(std::move(outer_scan), std::move(inner_scan), std::move(pred));

    auto got = sorted(drain(join));
    std::vector<Row4> expected = {
        {1, 0, 2, 0},
        {1, 0, 4, 0},
        {2, 0, 4, 0},
        {3, 0, 4, 0},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(NestedLoopJoinTest, NoMatches) {
    auto l = make_kv_table("l", {{1, 10}, {2, 20}});
    auto r = make_kv_table("r", {{3, 30}, {4, 40}});

    auto outer_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    auto inner_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::EQ, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(2, TypeId::INT64));
    NestedLoopJoin join(std::move(outer_scan), std::move(inner_scan), std::move(pred));

    auto got = drain(join);
    EXPECT_TRUE(got.empty());
}

TEST_F(NestedLoopJoinTest, EmptyOuter) {
    Schema s = {{"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_empty", s);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());
    auto r = make_kv_table("r", {{1, 10}});

    auto outer_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    auto inner_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::EQ, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(2, TypeId::INT64));
    NestedLoopJoin join(std::move(outer_scan), std::move(inner_scan), std::move(pred));

    auto got = drain(join);
    EXPECT_TRUE(got.empty());
}

TEST_F(NestedLoopJoinTest, EmptyInner) {
    auto l = make_kv_table("l", {{1, 10}});
    Schema s = {{"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}};
    auto rt = Table::create(TEST_ROOT, "r_empty", s);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    auto outer_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    auto inner_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::EQ, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(2, TypeId::INT64));
    NestedLoopJoin join(std::move(outer_scan), std::move(inner_scan), std::move(pred));

    auto got = drain(join);
    EXPECT_TRUE(got.empty());
}

TEST_F(NestedLoopJoinTest, OutputChunksBoundedByCHUNK_SIZE) {
    std::vector<std::pair<i64, i64>> l_rows;
    for (i64 i = 0; i < 3; ++i)
        l_rows.push_back({1, i * 10});
    std::vector<std::pair<i64, i64>> r_rows;
    for (i64 i = 0; i < 1500; ++i)
        r_rows.push_back({1, i});
    auto l = make_kv_table("l", l_rows);
    auto r = make_kv_table("r", r_rows);

    auto outer_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    auto inner_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::EQ, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(2, TypeId::INT64));
    NestedLoopJoin join(std::move(outer_scan), std::move(inner_scan), std::move(pred));

    ASSERT_TRUE(join.open().is_ok());
    std::vector<size_t> chunk_sizes;
    size_t total = 0;
    while (true) {
        auto n = join.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        chunk_sizes.push_back(n.value()->row_count());
        total += n.value()->row_count();
    }
    join.close();
    EXPECT_EQ(total, static_cast<size_t>(3 * 1500));
    for (size_t s : chunk_sizes)
        EXPECT_LE(s, static_cast<size_t>(TableScan::CHUNK_SIZE));
}

TEST_F(NestedLoopJoinTest, OutputSchemaMatchesConcat) {
    Schema ls = {{"la", TypeId::INT64, false}, {"lb", TypeId::INT64, false}};
    auto lt = Table::create(TEST_ROOT, "l_schema", ls);
    ASSERT_TRUE(lt.is_ok());
    auto l = std::move(lt.value());
    Schema rs = {{"ra", TypeId::INT64, false}, {"rb", TypeId::INT64, false}};
    auto rt = Table::create(TEST_ROOT, "r_schema", rs);
    ASSERT_TRUE(rt.is_ok());
    auto r = std::move(rt.value());

    auto outer_scan = std::make_unique<TableScan>(&l, std::vector<size_t>{0, 1});
    auto inner_scan = std::make_unique<TableScan>(&r, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::EQ, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<ColumnRef>(2, TypeId::INT64));
    NestedLoopJoin join(std::move(outer_scan), std::move(inner_scan), std::move(pred));

    const Schema& os = join.output_schema();
    ASSERT_EQ(os.size(), 4u);
    EXPECT_EQ(os[0].name, "la");
    EXPECT_EQ(os[1].name, "lb");
    EXPECT_EQ(os[2].name, "ra");
    EXPECT_EQ(os[3].name, "rb");
}
