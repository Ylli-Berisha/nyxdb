#include "executor/chunk.h"
#include "executor/column_vector.h"
#include "executor/expression.h"
#include "executor/filter.h"
#include "executor/table_scan.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_filter_test";

static ColumnVector make_mask_i32(const std::vector<i32>& bits, bool nullable = false,
                                  const std::vector<size_t>& null_positions = {}) {
    auto m = ColumnVector::make(TypeId::INT32, bits.size(), nullable);
    for (size_t i = 0; i < bits.size(); ++i)
        m.set_i32(i, bits[i]);
    for (size_t p : null_positions)
        m.set_null(p);
    return m;
}

TEST(ColumnVectorCompactTest, AllPass) {
    auto v = ColumnVector::make(TypeId::INT64, 5, false);
    for (i64 i = 0; i < 5; ++i)
        v.set_i64(i, i * 10);
    auto mask = make_mask_i32({1, 1, 1, 1, 1});
    v.compact_in_place(mask);
    EXPECT_EQ(v.size(), 5u);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_EQ(v.get_i64(i), static_cast<i64>(i) * 10);
}

TEST(ColumnVectorCompactTest, AllFail) {
    auto v = ColumnVector::make(TypeId::INT64, 5, false);
    for (i64 i = 0; i < 5; ++i)
        v.set_i64(i, i);
    auto mask = make_mask_i32({0, 0, 0, 0, 0});
    v.compact_in_place(mask);
    EXPECT_EQ(v.size(), 0u);
}

TEST(ColumnVectorCompactTest, Mixed) {
    auto v = ColumnVector::make(TypeId::INT64, 5, false);
    for (i64 i = 0; i < 5; ++i)
        v.set_i64(i, i + 100);
    auto mask = make_mask_i32({1, 0, 1, 0, 1});
    v.compact_in_place(mask);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v.get_i64(0), 100);
    EXPECT_EQ(v.get_i64(1), 102);
    EXPECT_EQ(v.get_i64(2), 104);
}

TEST(ColumnVectorCompactTest, WithNullsInData) {
    auto v = ColumnVector::make(TypeId::INT64, 6, true);
    for (i64 i = 0; i < 6; ++i)
        v.set_i64(i, i * 5);
    v.set_null(1);
    v.set_null(4);
    // mask keeps rows 0, 1, 3, 4 (i.e., 4 rows). Nulls at input 1, 4 should be at output 1, 3.
    auto mask = make_mask_i32({1, 1, 0, 1, 1, 0});
    v.compact_in_place(mask);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_FALSE(v.is_null(0));
    EXPECT_EQ(v.get_i64(0), 0);
    EXPECT_TRUE(v.is_null(1));
    EXPECT_FALSE(v.is_null(2));
    EXPECT_EQ(v.get_i64(2), 15);
    EXPECT_TRUE(v.is_null(3));
}

TEST(ColumnVectorCompactTest, WithNullsInMask) {
    auto v = ColumnVector::make(TypeId::INT64, 5, false);
    for (i64 i = 0; i < 5; ++i)
        v.set_i64(i, i + 1);
    // Mask [1, null, 1, 0, 1] — nullable mask. Null → drop.
    auto mask = make_mask_i32({1, 1, 1, 0, 1}, true, {1});
    v.compact_in_place(mask);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v.get_i64(0), 1);
    EXPECT_EQ(v.get_i64(1), 3);
    EXPECT_EQ(v.get_i64(2), 5);
}

TEST(ChunkCompactTest, RowCountInvariantPreserved) {
    auto c0 = ColumnVector::make(TypeId::INT32, 4, false);
    auto c1 = ColumnVector::make(TypeId::INT64, 4, false);
    auto c2 = ColumnVector::make(TypeId::DOUBLE, 4, true);
    for (size_t i = 0; i < 4; ++i) {
        c0.set_i32(i, static_cast<i32>(i));
        c1.set_i64(i, static_cast<i64>(i) * 100);
        c2.set_f64(i, static_cast<f64>(i) + 0.5);
    }
    c2.set_null(2);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(c0));
    cols.push_back(std::move(c1));
    cols.push_back(std::move(c2));
    Chunk chunk(4, std::move(cols));

    auto mask = make_mask_i32({0, 1, 1, 0});
    chunk.compact_in_place(mask);

    EXPECT_EQ(chunk.row_count(), 2u);
    EXPECT_EQ(chunk.column(0).size(), 2u);
    EXPECT_EQ(chunk.column(1).size(), 2u);
    EXPECT_EQ(chunk.column(2).size(), 2u);
    EXPECT_EQ(chunk.column(0).get_i32(0), 1);
    EXPECT_EQ(chunk.column(1).get_i64(1), 200);
    EXPECT_TRUE(chunk.column(2).is_null(1));
}

class FilterTest : public ::testing::Test {
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
};

TEST_F(FilterTest, AllPass) {
    std::vector<i64> values;
    for (i64 i = 1; i <= 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(0)}));
    Filter f(std::move(scan), std::move(pred));
    ASSERT_TRUE(f.open().is_ok());

    size_t total = 0;
    while (true) {
        auto n = f.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        total += n.value()->row_count();
    }
    EXPECT_EQ(total, 100u);
}

TEST_F(FilterTest, AllFail) {
    std::vector<i64> values;
    for (i64 i = 0; i < 100; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1000)}));
    Filter f(std::move(scan), std::move(pred));
    ASSERT_TRUE(f.open().is_ok());

    auto n = f.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(FilterTest, Selective) {
    std::vector<i64> values;
    for (i64 i = 0; i < 1000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    // predicate: v > 499
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(499)}));
    Filter f(std::move(scan), std::move(pred));
    ASSERT_TRUE(f.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = f.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(got.size(), 500u);
    for (size_t i = 0; i < 500; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(500 + i));
}

TEST_F(FilterTest, AcrossChunks) {
    std::vector<i64> values;
    for (i64 i = 0; i < 3000; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values);

    // predicate: v > 1500 → keeps 1501..2999 = 1499 rows
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(1500)}));
    Filter f(std::move(scan), std::move(pred));
    ASSERT_TRUE(f.open().is_ok());

    std::vector<i64> got;
    while (true) {
        auto n = f.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    ASSERT_EQ(got.size(), 1499u);
    for (size_t i = 0; i < 1499; ++i)
        EXPECT_EQ(got[i], static_cast<i64>(1501 + i));
}

TEST_F(FilterTest, NullsInPredicateDropRows) {
    // Values: 1..10, but positions {2, 5, 7} are null.
    std::vector<i64> values;
    for (i64 i = 1; i <= 10; ++i)
        values.push_back(i);
    auto t = make_int64_table("t", values, true, {2, 5, 7});

    // predicate: v > 3. For null input, LT produces null → dropped.
    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(3)}));
    Filter f(std::move(scan), std::move(pred));
    ASSERT_TRUE(f.open().is_ok());

    // Keep values > 3 at non-null positions: originals at (0-indexed) 3,4,6,8,9 = values
    // 4,5,7,9,10.
    std::vector<i64> expected = {4, 5, 7, 9, 10};
    std::vector<i64> got;
    while (true) {
        auto n = f.next();
        ASSERT_TRUE(n.is_ok());
        if (!n.value().has_value())
            break;
        const Chunk& c = *n.value();
        for (size_t i = 0; i < c.row_count(); ++i)
            got.push_back(c.column(0).get_i64(i));
    }
    EXPECT_EQ(got, expected);
}

TEST_F(FilterTest, EmptyChildYieldsNothing) {
    Schema s = {{"v", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "empty", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(0, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(0)}));
    Filter f(std::move(scan), std::move(pred));
    ASSERT_TRUE(f.open().is_ok());
    auto n = f.next();
    ASSERT_TRUE(n.is_ok());
    EXPECT_FALSE(n.value().has_value());
}

TEST_F(FilterTest, OutputSchemaMatchesChild) {
    Schema s = {{"a", TypeId::INT32, false}, {"b", TypeId::INT64, false}};
    auto tres = Table::create(TEST_ROOT, "twocol", s);
    ASSERT_TRUE(tres.is_ok());
    auto t = std::move(tres.value());

    auto scan = std::make_unique<TableScan>(&t, std::vector<size_t>{0, 1});
    auto pred =
        std::make_unique<BinaryOp>(BinaryOpKind::GT, std::make_unique<ColumnRef>(1, TypeId::INT64),
                                   std::make_unique<Literal>(Value{static_cast<i64>(0)}));
    Filter f(std::move(scan), std::move(pred));
    const Schema& os = f.output_schema();
    ASSERT_EQ(os.size(), 2u);
    EXPECT_EQ(os[0].name, "a");
    EXPECT_EQ(os[1].name, "b");
}
