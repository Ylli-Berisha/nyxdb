#include "executor/chunk.h"

#include <gtest/gtest.h>

using namespace nyx;

TEST(ChunkTest, EmptyChunk) {
    Chunk c(0, {});
    EXPECT_EQ(c.row_count(), 0u);
    EXPECT_EQ(c.column_count(), 0u);
}

TEST(ChunkTest, SingleColumnRoundtrip) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, false);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i * i);

    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(10, std::move(cols));

    EXPECT_EQ(c.row_count(), 10u);
    EXPECT_EQ(c.column_count(), 1u);
    for (i64 i = 0; i < 10; ++i)
        EXPECT_EQ(c.column(0).get_i64(static_cast<size_t>(i)), i * i);
}

TEST(ChunkTest, MixedTypesChunk) {
    auto a = ColumnVector::make(TypeId::INT32, 5, false);
    auto b = ColumnVector::make(TypeId::INT64, 5, false);
    auto d = ColumnVector::make(TypeId::DOUBLE, 5, true);
    for (size_t i = 0; i < 5; ++i) {
        a.set_i32(i, static_cast<i32>(i));
        b.set_i64(i, static_cast<i64>(i) * 100);
        d.set_f64(i, static_cast<f64>(i) + 0.5);
    }
    d.set_null(2);

    std::vector<ColumnVector> cols;
    cols.push_back(std::move(a));
    cols.push_back(std::move(b));
    cols.push_back(std::move(d));
    Chunk c(5, std::move(cols));

    EXPECT_EQ(c.column_count(), 3u);
    EXPECT_EQ(c.row_count(), 5u);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(c.column(0).get_i32(i), static_cast<i32>(i));
        EXPECT_EQ(c.column(1).get_i64(i), static_cast<i64>(i) * 100);
        if (i == 2) {
            EXPECT_TRUE(c.column(2).is_null(i));
        } else {
            EXPECT_DOUBLE_EQ(c.column(2).get_f64(i), static_cast<f64>(i) + 0.5);
        }
    }
}

TEST(ChunkTest, ColumnMutableAccess) {
    auto cv = ColumnVector::make(TypeId::INT32, 3, false);
    cv.set_i32(0, 10);
    cv.set_i32(1, 20);
    cv.set_i32(2, 30);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(3, std::move(cols));

    c.column(0).set_i32(1, 999);
    EXPECT_EQ(c.column(0).get_i32(1), 999);
}

TEST(ChunkTest, RangeForYieldsColumns) {
    std::vector<ColumnVector> cols;
    cols.push_back(ColumnVector::make(TypeId::INT32, 2, false));
    cols.push_back(ColumnVector::make(TypeId::INT64, 2, false));
    cols.push_back(ColumnVector::make(TypeId::DOUBLE, 2, false));
    Chunk c(2, std::move(cols));

    std::vector<TypeId> seen;
    for (const auto& col : c)
        seen.push_back(col.type());

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], TypeId::INT32);
    EXPECT_EQ(seen[1], TypeId::INT64);
    EXPECT_EQ(seen[2], TypeId::DOUBLE);
}

TEST(ChunkTest, MoveConstructionPreservesData) {
    auto cv = ColumnVector::make(TypeId::INT64, 4, false);
    for (i64 i = 0; i < 4; ++i)
        cv.set_i64(static_cast<size_t>(i), i + 42);

    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk a(4, std::move(cols));

    Chunk b = std::move(a);
    EXPECT_EQ(b.row_count(), 4u);
    EXPECT_EQ(b.column_count(), 1u);
    for (i64 i = 0; i < 4; ++i)
        EXPECT_EQ(b.column(0).get_i64(static_cast<size_t>(i)), i + 42);
}
