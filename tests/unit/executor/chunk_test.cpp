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

TEST(ChunkSelTest, DefaultsToNoSel) {
    auto cv = ColumnVector::make(TypeId::INT64, 5, false);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(5, std::move(cols));
    EXPECT_FALSE(c.has_sel());
    EXPECT_EQ(c.logical_size(), 5u);
    EXPECT_EQ(c.row_count(), 5u);
}

TEST(ChunkSelTest, SetSelChangesLogicalSize) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, false);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(10, std::move(cols));

    c.set_sel(SelectionVector::from_dense({1, 3, 5, 7}, 10));
    EXPECT_TRUE(c.has_sel());
    EXPECT_EQ(c.logical_size(), 4u);
    EXPECT_EQ(c.row_count(), 10u);
    EXPECT_EQ(c.sel().at(0), 1u);
    EXPECT_EQ(c.sel().at(3), 7u);
}

TEST(ChunkSelTest, MaterializeGathersLiveRows) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, true);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i * 10);
    cv.set_null(3);
    cv.set_null(7);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(10, std::move(cols));

    c.set_sel(SelectionVector::from_dense({2, 3, 5, 7}, 10));
    c.materialize();

    EXPECT_FALSE(c.has_sel());
    EXPECT_EQ(c.row_count(), 4u);
    EXPECT_EQ(c.logical_size(), 4u);
    EXPECT_FALSE(c.column(0).is_null(0));
    EXPECT_EQ(c.column(0).get_i64(0), 20);
    EXPECT_TRUE(c.column(0).is_null(1));
    EXPECT_FALSE(c.column(0).is_null(2));
    EXPECT_EQ(c.column(0).get_i64(2), 50);
    EXPECT_TRUE(c.column(0).is_null(3));
}

TEST(ChunkSelTest, MaterializeIsNoOpWhenNoSel) {
    auto cv = ColumnVector::make(TypeId::INT32, 3, false);
    cv.set_i32(0, 10);
    cv.set_i32(1, 20);
    cv.set_i32(2, 30);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(3, std::move(cols));

    c.materialize();
    EXPECT_EQ(c.row_count(), 3u);
    EXPECT_EQ(c.column(0).get_i32(0), 10);
    EXPECT_EQ(c.column(0).get_i32(2), 30);
}

TEST(ChunkSelTest, CompactFromSelFiltersLiveRows) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, false);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(10, std::move(cols));

    c.set_sel(SelectionVector::from_dense({0, 2, 4, 6, 8}, 10));

    auto mask = ColumnVector::make(TypeId::INT32, 5, false);
    mask.set_i32(0, 0);
    mask.set_i32(1, 1);
    mask.set_i32(2, 0);
    mask.set_i32(3, 1);
    mask.set_i32(4, 1);
    c.compact_from_sel(mask);

    EXPECT_FALSE(c.has_sel());
    ASSERT_EQ(c.row_count(), 3u);
    EXPECT_EQ(c.column(0).get_i64(0), 2);
    EXPECT_EQ(c.column(0).get_i64(1), 6);
    EXPECT_EQ(c.column(0).get_i64(2), 8);
}

TEST(ChunkSelTest, ResizeWithSelTruncatesSel) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, false);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(10, std::move(cols));

    c.set_sel(SelectionVector::from_dense({1, 3, 5, 7, 9}, 10));
    c.resize(3);
    EXPECT_TRUE(c.has_sel());
    EXPECT_EQ(c.logical_size(), 3u);
    EXPECT_EQ(c.row_count(), 10u);
    EXPECT_EQ(c.sel().at(0), 1u);
    EXPECT_EQ(c.sel().at(2), 5u);
}

TEST(ChunkSelTest, DropPrefixWithSelDropsSelPrefix) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, false);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(10, std::move(cols));

    c.set_sel(SelectionVector::from_dense({1, 3, 5, 7, 9}, 10));
    c.drop_prefix(2);
    EXPECT_TRUE(c.has_sel());
    EXPECT_EQ(c.logical_size(), 3u);
    EXPECT_EQ(c.row_count(), 10u);
    EXPECT_EQ(c.sel().at(0), 5u);
    EXPECT_EQ(c.sel().at(2), 9u);
}

TEST(ChunkSelTest, ResizeWithoutSelBehavesPhysically) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, false);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i);
    std::vector<ColumnVector> cols;
    cols.push_back(std::move(cv));
    Chunk c(10, std::move(cols));

    c.resize(4);
    EXPECT_FALSE(c.has_sel());
    EXPECT_EQ(c.row_count(), 4u);
    EXPECT_EQ(c.column(0).size(), 4u);
    EXPECT_EQ(c.column(0).get_i64(3), 3);
}

TEST(ColumnVectorGatherTest, ViaSelWithNulls) {
    auto cv = ColumnVector::make(TypeId::INT64, 8, true);
    for (i64 i = 0; i < 8; ++i)
        cv.set_i64(static_cast<size_t>(i), i * 100);
    cv.set_null(2);
    cv.set_null(5);

    auto sel = SelectionVector::from_dense({0, 2, 3, 5, 6}, 8);
    auto out = ColumnVector::gather_via_sel(cv, sel);

    ASSERT_EQ(out.size(), 5u);
    EXPECT_TRUE(out.nullable());
    EXPECT_TRUE(out.has_nulls());
    EXPECT_FALSE(out.is_null(0));
    EXPECT_EQ(out.get_i64(0), 0);
    EXPECT_TRUE(out.is_null(1));
    EXPECT_FALSE(out.is_null(2));
    EXPECT_EQ(out.get_i64(2), 300);
    EXPECT_TRUE(out.is_null(3));
    EXPECT_FALSE(out.is_null(4));
    EXPECT_EQ(out.get_i64(4), 600);
}
