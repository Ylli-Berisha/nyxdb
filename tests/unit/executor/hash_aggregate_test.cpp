#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/filter.h"
#include "executor/hash_aggregate.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_hash_agg_test";

class HashAggregateTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TEST_ROOT); }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    Table make_i64_table(const std::string& name, const std::vector<i64>& values) {
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

    static std::vector<AggregateSpec> count_star_spec() {
        std::vector<AggregateSpec> specs;
        specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
        return specs;
    }

    static i64 drain_single_i64(Operator& op) {
        EXPECT_TRUE(op.open().is_ok());
        i64 result = -1;
        size_t row_count = 0;
        while (true) {
            auto n = op.next();
            EXPECT_TRUE(n.is_ok());
            if (!n.value().has_value())
                break;
            const Chunk& c = *n.value();
            for (size_t i = 0; i < c.row_count(); ++i) {
                result = c.column(0).get_i64(i);
                ++row_count;
            }
        }
        op.close();
        EXPECT_EQ(row_count, 1u);
        return result;
    }
};

TEST_F(HashAggregateTest, CountStarBasic) {
    std::vector<i64> input;
    for (i64 i = 0; i < 100; ++i)
        input.push_back(i);
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    HashAggregate agg(std::move(scan), {}, count_star_spec());

    EXPECT_EQ(drain_single_i64(agg), 100);
}

TEST_F(HashAggregateTest, CountStarEmpty) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "empty", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    HashAggregate agg(std::move(scan), {}, count_star_spec());

    EXPECT_EQ(drain_single_i64(agg), 0);
}

TEST_F(HashAggregateTest, CountStarMultipleChunks) {
    std::vector<i64> input;
    for (i64 i = 0; i < 3000; ++i)
        input.push_back(i);
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    HashAggregate agg(std::move(scan), {}, count_star_spec());

    EXPECT_EQ(drain_single_i64(agg), 3000);
}

TEST_F(HashAggregateTest, CountStarSelVecInput) {
    std::vector<i64> input;
    for (i64 i = 0; i < 100; ++i)
        input.push_back(i);
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GE, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(50)}));
    auto filter = std::make_unique<Filter>(std::move(scan), std::move(pred),
                                           FilterStrategy::SELECTION_VECTOR);
    HashAggregate agg(std::move(filter), {}, count_star_spec());

    EXPECT_EQ(drain_single_i64(agg), 50);
}

TEST_F(HashAggregateTest, CountExprSkipsNulls) {
    Schema s = {{"v", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "cnull", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (i64 i = 0; i < 10; ++i) {
        bool null_at = (i % 3 == 0);
        rows.push_back({null_at ? Value{std::monostate{}} : Value{i}});
    }
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::COUNT, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    EXPECT_EQ(drain_single_i64(agg), 6);
}

TEST_F(HashAggregateTest, SumInt64) {
    std::vector<i64> input = {1, 2, 3, 4, 5};
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    EXPECT_EQ(drain_single_i64(agg), 15);
}

TEST_F(HashAggregateTest, SumInt32WidensToInt64) {
    Schema s = {{"v", TypeId::INT32, false}};
    auto tres = Table::create(TEST_ROOT, "s32", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (i32 i = 1; i <= 10; ++i)
        rows.push_back({Value{i}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(0, TypeId::INT32)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_EQ(agg.output_schema().size(), 1u);
    EXPECT_EQ(agg.output_schema()[0].type, TypeId::INT64);
    EXPECT_TRUE(agg.output_schema()[0].nullable);

    EXPECT_EQ(drain_single_i64(agg), 55);
}

TEST_F(HashAggregateTest, SumDouble) {
    Schema s = {{"v", TypeId::DOUBLE, false}};
    auto tres = Table::create(TEST_ROOT, "sd", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    rows.push_back({Value{1.5}});
    rows.push_back({Value{2.5}});
    rows.push_back({Value{3.0}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(0, TypeId::DOUBLE)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    ASSERT_FALSE(c.column(0).is_null(0));
    EXPECT_DOUBLE_EQ(c.column(0).get_f64(0), 7.0);
    agg.close();
}

TEST_F(HashAggregateTest, SumAllNullsIsNull) {
    Schema s = {{"v", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "san", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (int i = 0; i < 5; ++i)
        rows.push_back({Value{std::monostate{}}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_TRUE(c.column(0).is_null(0));
    agg.close();
}

TEST_F(HashAggregateTest, SumEmptyIsNull) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "se", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_TRUE(c.column(0).is_null(0));
    agg.close();
}

TEST_F(HashAggregateTest, MinMaxInt64) {
    std::vector<i64> input = {7, 3, 9, 1, 5};
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::MIN, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MAX, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_EQ(c.column(0).get_i64(0), 1);
    EXPECT_EQ(c.column(1).get_i64(0), 9);
    agg.close();
}

TEST_F(HashAggregateTest, MinMaxDouble) {
    Schema s = {{"v", TypeId::DOUBLE, false}};
    auto tres = Table::create(TEST_ROOT, "mmd", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (f64 v : {2.5, 1.5, 3.5, 0.5}) {
        rows.push_back({Value{v}});
    }
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::MIN, std::make_unique<ColumnRef>(0, TypeId::DOUBLE)});
    specs.push_back(
        AggregateSpec{AggregateKind::MAX, std::make_unique<ColumnRef>(0, TypeId::DOUBLE)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_DOUBLE_EQ(c.column(0).get_f64(0), 0.5);
    EXPECT_DOUBLE_EQ(c.column(1).get_f64(0), 3.5);
    agg.close();
}

TEST_F(HashAggregateTest, MinMaxAllNullsIsNull) {
    Schema s = {{"v", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "mmnull", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (int i = 0; i < 3; ++i)
        rows.push_back({Value{std::monostate{}}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::MIN, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MAX, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_TRUE(c.column(0).is_null(0));
    EXPECT_TRUE(c.column(1).is_null(0));
    agg.close();
}

TEST_F(HashAggregateTest, MultipleAggregatesTogether) {
    std::vector<i64> input = {1, 2, 3, 4, 5};
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MIN, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MAX, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    ASSERT_EQ(c.column_count(), 4u);
    EXPECT_EQ(c.column(0).get_i64(0), 5);
    EXPECT_EQ(c.column(1).get_i64(0), 15);
    EXPECT_EQ(c.column(2).get_i64(0), 1);
    EXPECT_EQ(c.column(3).get_i64(0), 5);
    agg.close();
}

TEST_F(HashAggregateTest, AvgInt64) {
    std::vector<i64> input = {1, 2, 3, 4, 5};
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_EQ(agg.output_schema().size(), 1u);
    EXPECT_EQ(agg.output_schema()[0].type, TypeId::DOUBLE);
    EXPECT_TRUE(agg.output_schema()[0].nullable);

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    ASSERT_FALSE(c.column(0).is_null(0));
    EXPECT_DOUBLE_EQ(c.column(0).get_f64(0), 3.0);
    agg.close();
}

TEST_F(HashAggregateTest, AvgInt32) {
    Schema s = {{"v", TypeId::INT32, false}};
    auto tres = Table::create(TEST_ROOT, "avg32", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (i32 i = 1; i <= 10; ++i)
        rows.push_back({Value{i}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(0, TypeId::INT32)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_DOUBLE_EQ(c.column(0).get_f64(0), 5.5);
    agg.close();
}

TEST_F(HashAggregateTest, AvgDouble) {
    Schema s = {{"v", TypeId::DOUBLE, false}};
    auto tres = Table::create(TEST_ROOT, "avgd", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (f64 v : {2.0, 4.0, 6.0}) {
        rows.push_back({Value{v}});
    }
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(0, TypeId::DOUBLE)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_DOUBLE_EQ(c.column(0).get_f64(0), 4.0);
    agg.close();
}

TEST_F(HashAggregateTest, AvgSkipsNulls) {
    Schema s = {{"v", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "avgnull", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    rows.push_back({Value{static_cast<i64>(2)}});
    rows.push_back({Value{std::monostate{}}});
    rows.push_back({Value{static_cast<i64>(4)}});
    rows.push_back({Value{std::monostate{}}});
    rows.push_back({Value{static_cast<i64>(6)}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_DOUBLE_EQ(c.column(0).get_f64(0), 4.0);
    agg.close();
}

TEST_F(HashAggregateTest, AvgAllNullsIsNull) {
    Schema s = {{"v", TypeId::INT64, true}};
    auto tres = Table::create(TEST_ROOT, "avgan", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (int i = 0; i < 3; ++i)
        rows.push_back({Value{std::monostate{}}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_TRUE(c.column(0).is_null(0));
    agg.close();
}

TEST_F(HashAggregateTest, AvgEmptyIsNull) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "avge", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    EXPECT_TRUE(c.column(0).is_null(0));
    agg.close();
}

TEST_F(HashAggregateTest, AvgMixedWithOtherAggregates) {
    std::vector<i64> input = {2, 4, 6, 8, 10};
    auto t = make_i64_table("t", input);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    std::vector<AggregateSpec> specs;
    specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(0, TypeId::INT64)});
    HashAggregate agg(std::move(scan), {}, std::move(specs));

    ASSERT_EQ(agg.output_schema().size(), 3u);
    EXPECT_EQ(agg.output_schema()[0].name, "count_star");
    EXPECT_EQ(agg.output_schema()[1].name, "avg");
    EXPECT_EQ(agg.output_schema()[2].name, "sum");

    ASSERT_TRUE(agg.open().is_ok());
    auto n = agg.next();
    ASSERT_TRUE(n.is_ok());
    ASSERT_TRUE(n.value().has_value());
    const Chunk& c = *n.value();
    ASSERT_EQ(c.row_count(), 1u);
    ASSERT_EQ(c.column_count(), 3u);
    EXPECT_EQ(c.column(0).get_i64(0), 5);
    EXPECT_DOUBLE_EQ(c.column(1).get_f64(0), 6.0);
    EXPECT_EQ(c.column(2).get_i64(0), 30);
    agg.close();
}

TEST_F(HashAggregateTest, GroupBySingleKeyCount) {
    Schema s = {{"cat", TypeId::INT32, false}, {"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "gsk", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    for (i32 i = 0; i < 100; ++i)
        rows.push_back({Value{i % 4}, Value{static_cast<i64>(i)}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    std::vector<std::unique_ptr<Expression>> keys;
    keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT32));
    std::vector<AggregateSpec> specs;
    specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
    HashAggregate agg(std::move(scan), std::move(keys), std::move(specs));

    ASSERT_EQ(agg.output_schema().size(), 2u);
    EXPECT_EQ(agg.output_schema()[0].name, "key_0");
    EXPECT_EQ(agg.output_schema()[0].type, TypeId::INT32);
    EXPECT_EQ(agg.output_schema()[1].name, "count_star");

    ASSERT_TRUE(agg.open().is_ok());
    std::vector<std::pair<i32, i64>> got;
    while (true) {
        auto n = agg.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i32(i), c.column(1).get_i64(i));
    }
    agg.close();
    std::sort(got.begin(), got.end());
    std::vector<std::pair<i32, i64>> expected = {{0, 25}, {1, 25}, {2, 25}, {3, 25}};
    EXPECT_EQ(got, expected);
}

TEST_F(HashAggregateTest, GroupBySumMinMax) {
    Schema s = {{"cat", TypeId::INT32, false}, {"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "gsmm", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(10)}},
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(20)}},
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(30)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<i64>(100)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<i64>(200)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    std::vector<std::unique_ptr<Expression>> keys;
    keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT32));
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MIN, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MAX, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    HashAggregate agg(std::move(scan), std::move(keys), std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    std::vector<std::tuple<i32, i64, i64, i64>> got;
    while (true) {
        auto n = agg.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i32(i), c.column(1).get_i64(i), c.column(2).get_i64(i),
                             c.column(3).get_i64(i));
    }
    agg.close();
    std::sort(got.begin(), got.end());
    std::vector<std::tuple<i32, i64, i64, i64>> expected = {
        {1, 60, 10, 30},
        {2, 300, 100, 200},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(HashAggregateTest, GroupByMultiKey) {
    Schema s = {
        {"a", TypeId::INT64, false}, {"b", TypeId::INT64, false}, {"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "gmk", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(1)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(2)}},
        {Value{static_cast<i64>(1)}, Value{static_cast<i64>(20)}, Value{static_cast<i64>(3)}},
        {Value{static_cast<i64>(2)}, Value{static_cast<i64>(10)}, Value{static_cast<i64>(4)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1, 2});
    std::vector<std::unique_ptr<Expression>> keys;
    keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT64));
    keys.push_back(std::make_unique<ColumnRef>(1, TypeId::INT64));
    std::vector<AggregateSpec> specs;
    specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
    HashAggregate agg(std::move(scan), std::move(keys), std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    std::vector<std::tuple<i64, i64, i64>> got;
    while (true) {
        auto n = agg.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i),
                             c.column(2).get_i64(i));
    }
    agg.close();
    std::sort(got.begin(), got.end());
    std::vector<std::tuple<i64, i64, i64>> expected = {{1, 10, 2}, {1, 20, 1}, {2, 10, 1}};
    EXPECT_EQ(got, expected);
}

TEST_F(HashAggregateTest, GroupByMixedTypes) {
    Schema s = {
        {"a", TypeId::INT32, false}, {"b", TypeId::DOUBLE, false}, {"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "gmt", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i32>(1)}, Value{1.5}, Value{static_cast<i64>(10)}},
        {Value{static_cast<i32>(1)}, Value{1.5}, Value{static_cast<i64>(20)}},
        {Value{static_cast<i32>(2)}, Value{2.5}, Value{static_cast<i64>(30)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1, 2});
    std::vector<std::unique_ptr<Expression>> keys;
    keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT32));
    keys.push_back(std::make_unique<ColumnRef>(1, TypeId::DOUBLE));
    std::vector<AggregateSpec> specs;
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(2, TypeId::INT64)});
    HashAggregate agg(std::move(scan), std::move(keys), std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    std::vector<std::tuple<i32, f64, i64>> got;
    while (true) {
        auto n = agg.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i32(i), c.column(1).get_f64(i),
                             c.column(2).get_i64(i));
    }
    agg.close();
    std::sort(got.begin(), got.end(),
              [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(std::get<0>(got[0]), 1);
    EXPECT_DOUBLE_EQ(std::get<1>(got[0]), 1.5);
    EXPECT_EQ(std::get<2>(got[0]), 30);
    EXPECT_EQ(std::get<0>(got[1]), 2);
    EXPECT_DOUBLE_EQ(std::get<1>(got[1]), 2.5);
    EXPECT_EQ(std::get<2>(got[1]), 30);
}

TEST_F(HashAggregateTest, GroupByNullFormsGroup) {
    Schema s = {{"cat", TypeId::INT32, true}, {"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "gnfg", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(10)}},
        {Value{std::monostate{}}, Value{static_cast<i64>(20)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<i64>(30)}},
        {Value{std::monostate{}}, Value{static_cast<i64>(40)}},
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(50)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    std::vector<std::unique_ptr<Expression>> keys;
    keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT32));
    std::vector<AggregateSpec> specs;
    specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    HashAggregate agg(std::move(scan), std::move(keys), std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    std::vector<std::tuple<bool, i32, i64, i64>> got;
    while (true) {
        auto n = agg.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i) {
            bool null = c.column(0).is_null(i);
            got.emplace_back(null, null ? 0 : c.column(0).get_i32(i), c.column(1).get_i64(i),
                             c.column(2).get_i64(i));
        }
    }
    agg.close();
    std::sort(got.begin(), got.end());
    std::vector<std::tuple<bool, i32, i64, i64>> expected = {
        {false, 1, 2, 60},
        {false, 2, 1, 30},
        {true, 0, 2, 60},
    };
    EXPECT_EQ(got, expected);
}

TEST_F(HashAggregateTest, GroupByLargeResize) {
    Schema s = {{"cat", TypeId::INT64, false}, {"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "glr", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows;
    rows.reserve(10000);
    for (i64 i = 0; i < 10000; ++i)
        rows.push_back({Value{i}, Value{i * 2}});
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    std::vector<std::unique_ptr<Expression>> keys;
    keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT64));
    std::vector<AggregateSpec> specs;
    specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    HashAggregate agg(std::move(scan), std::move(keys), std::move(specs));

    ASSERT_TRUE(agg.open().is_ok());
    size_t total = 0;
    std::vector<std::tuple<i64, i64, i64>> got;
    got.reserve(10000);
    while (true) {
        auto n = agg.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i64(i), c.column(1).get_i64(i),
                             c.column(2).get_i64(i));
        total += c.row_count();
    }
    agg.close();
    ASSERT_EQ(total, 10000u);
    std::sort(got.begin(), got.end());
    for (size_t i = 0; i < 10000; ++i) {
        EXPECT_EQ(std::get<0>(got[i]), static_cast<i64>(i));
        EXPECT_EQ(std::get<1>(got[i]), 1);
        EXPECT_EQ(std::get<2>(got[i]), static_cast<i64>(i) * 2);
    }
}

TEST_F(HashAggregateTest, GroupByWithAllAggregates) {
    Schema s = {{"cat", TypeId::INT32, false}, {"x", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "gwa", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());
    std::vector<std::vector<Value>> rows = {
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(2)}},
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(4)}},
        {Value{static_cast<i32>(1)}, Value{static_cast<i64>(6)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<i64>(10)}},
        {Value{static_cast<i32>(2)}, Value{static_cast<i64>(20)}},
    };
    ASSERT_TRUE(t.insert_many(rows).is_ok());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    std::vector<std::unique_ptr<Expression>> keys;
    keys.push_back(std::make_unique<ColumnRef>(0, TypeId::INT32));
    std::vector<AggregateSpec> specs;
    specs.push_back(AggregateSpec{AggregateKind::COUNT_STAR, nullptr});
    specs.push_back(
        AggregateSpec{AggregateKind::COUNT, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::SUM, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::AVG, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MIN, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    specs.push_back(
        AggregateSpec{AggregateKind::MAX, std::make_unique<ColumnRef>(1, TypeId::INT64)});
    HashAggregate agg(std::move(scan), std::move(keys), std::move(specs));

    ASSERT_EQ(agg.output_schema().size(), 7u);

    ASSERT_TRUE(agg.open().is_ok());
    std::vector<std::tuple<i32, i64, i64, i64, f64, i64, i64>> got;
    while (true) {
        auto n = agg.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        ASSERT_EQ(c.column_count(), 7u);
        for (size_t i = 0; i < c.row_count(); ++i)
            got.emplace_back(c.column(0).get_i32(i), c.column(1).get_i64(i), c.column(2).get_i64(i),
                             c.column(3).get_i64(i), c.column(4).get_f64(i), c.column(5).get_i64(i),
                             c.column(6).get_i64(i));
    }
    agg.close();
    std::sort(got.begin(), got.end(),
              [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
    ASSERT_EQ(got.size(), 2u);
    // Group cat=1: rows 2,4,6 → count_star=3, count=3, sum=12, avg=4, min=2, max=6
    EXPECT_EQ(std::get<0>(got[0]), 1);
    EXPECT_EQ(std::get<1>(got[0]), 3);
    EXPECT_EQ(std::get<2>(got[0]), 3);
    EXPECT_EQ(std::get<3>(got[0]), 12);
    EXPECT_DOUBLE_EQ(std::get<4>(got[0]), 4.0);
    EXPECT_EQ(std::get<5>(got[0]), 2);
    EXPECT_EQ(std::get<6>(got[0]), 6);
    // Group cat=2: rows 10,20 → count_star=2, count=2, sum=30, avg=15, min=10, max=20
    EXPECT_EQ(std::get<0>(got[1]), 2);
    EXPECT_EQ(std::get<1>(got[1]), 2);
    EXPECT_EQ(std::get<2>(got[1]), 2);
    EXPECT_EQ(std::get<3>(got[1]), 30);
    EXPECT_DOUBLE_EQ(std::get<4>(got[1]), 15.0);
    EXPECT_EQ(std::get<5>(got[1]), 10);
    EXPECT_EQ(std::get<6>(got[1]), 20);
}

TEST_F(HashAggregateTest, OutputSchemaHasSingleCountColumn) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "schema_check", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    HashAggregate agg(std::move(scan), {}, count_star_spec());

    const Schema& os = agg.output_schema();
    ASSERT_EQ(os.size(), 1u);
    EXPECT_EQ(os[0].name, "count_star");
    EXPECT_EQ(os[0].type, TypeId::INT64);
    EXPECT_FALSE(os[0].nullable);
}
