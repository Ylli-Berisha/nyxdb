#include "executor/column_vector.h"

#include <gtest/gtest.h>

using namespace nyx;

TEST(ColumnVectorTest, MakeInt32NotNullable) {
    auto cv = ColumnVector::make(TypeId::INT32, 10, false);
    EXPECT_EQ(cv.type(), TypeId::INT32);
    EXPECT_EQ(cv.size(), 10u);
    EXPECT_FALSE(cv.nullable());
    EXPECT_FALSE(cv.has_nulls());
    EXPECT_EQ(cv.null_bitmap_bytes(), 0u);
}

TEST(ColumnVectorTest, MakeNullableEmptyBitmap) {
    auto cv = ColumnVector::make(TypeId::INT64, 16, true);
    EXPECT_TRUE(cv.nullable());
    EXPECT_FALSE(cv.has_nulls());
    EXPECT_EQ(cv.null_bitmap_bytes(), 2u);
    for (size_t i = 0; i < 16; ++i)
        EXPECT_FALSE(cv.is_null(i));
}

TEST(ColumnVectorTest, SetAndGetInt32) {
    auto cv = ColumnVector::make(TypeId::INT32, 5, false);
    for (i32 i = 0; i < 5; ++i)
        cv.set_i32(static_cast<size_t>(i), i * 7);
    for (i32 i = 0; i < 5; ++i)
        EXPECT_EQ(cv.get_i32(static_cast<size_t>(i)), i * 7);
}

TEST(ColumnVectorTest, SetAndGetInt64) {
    auto cv = ColumnVector::make(TypeId::INT64, 5, false);
    for (i64 i = 0; i < 5; ++i)
        cv.set_i64(static_cast<size_t>(i), i * 1000000LL + 3);
    for (i64 i = 0; i < 5; ++i)
        EXPECT_EQ(cv.get_i64(static_cast<size_t>(i)), i * 1000000LL + 3);
}

TEST(ColumnVectorTest, SetAndGetDouble) {
    auto cv = ColumnVector::make(TypeId::DOUBLE, 5, false);
    for (size_t i = 0; i < 5; ++i)
        cv.set_f64(i, static_cast<f64>(i) + 0.25);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_DOUBLE_EQ(cv.get_f64(i), static_cast<f64>(i) + 0.25);
}

TEST(ColumnVectorTest, SetNullFlipsHasNulls) {
    auto cv = ColumnVector::make(TypeId::INT32, 4, true);
    EXPECT_FALSE(cv.has_nulls());
    cv.set_null(2);
    EXPECT_TRUE(cv.has_nulls());
    EXPECT_TRUE(cv.is_null(2));
    EXPECT_FALSE(cv.is_null(0));
    EXPECT_FALSE(cv.is_null(1));
    EXPECT_FALSE(cv.is_null(3));
}

TEST(ColumnVectorTest, AppendInt64Grows) {
    auto cv = ColumnVector::empty(TypeId::INT64, false);
    EXPECT_EQ(cv.size(), 0u);
    for (i64 i = 0; i < 5; ++i)
        cv.append_i64(i * 10);
    EXPECT_EQ(cv.size(), 5u);
    for (i64 i = 0; i < 5; ++i)
        EXPECT_EQ(cv.get_i64(static_cast<size_t>(i)), i * 10);
}

TEST(ColumnVectorTest, AppendAlternatingWithNulls) {
    auto cv = ColumnVector::empty(TypeId::INT64, true);
    cv.append_i64(100);
    cv.append_null();
    cv.append_i64(200);
    cv.append_null();
    EXPECT_EQ(cv.size(), 4u);
    EXPECT_FALSE(cv.is_null(0));
    EXPECT_EQ(cv.get_i64(0), 100);
    EXPECT_TRUE(cv.is_null(1));
    EXPECT_FALSE(cv.is_null(2));
    EXPECT_EQ(cv.get_i64(2), 200);
    EXPECT_TRUE(cv.is_null(3));
    EXPECT_TRUE(cv.has_nulls());
}

TEST(ColumnVectorTest, ResizeShrinks) {
    auto cv = ColumnVector::make(TypeId::INT64, 10, false);
    for (i64 i = 0; i < 10; ++i)
        cv.set_i64(static_cast<size_t>(i), i);
    cv.resize(5);
    EXPECT_EQ(cv.size(), 5u);
    for (i64 i = 0; i < 5; ++i)
        EXPECT_EQ(cv.get_i64(static_cast<size_t>(i)), i);
}

TEST(ColumnVectorTest, ResizeGrows) {
    auto cv = ColumnVector::make(TypeId::INT64, 3, false);
    for (i64 i = 0; i < 3; ++i)
        cv.set_i64(static_cast<size_t>(i), i + 1);
    cv.resize(7);
    EXPECT_EQ(cv.size(), 7u);
    for (i64 i = 0; i < 3; ++i)
        EXPECT_EQ(cv.get_i64(static_cast<size_t>(i)), i + 1);
    for (size_t i = 3; i < 7; ++i)
        EXPECT_EQ(cv.get_i64(i), 0);
}

TEST(ColumnVectorTest, NullBitmapLayoutMatchesColumnPage) {
    auto cv = ColumnVector::make(TypeId::INT32, 12, true);
    cv.set_null(0);
    cv.set_null(3);
    cv.set_null(7);
    cv.set_null(8);
    ASSERT_EQ(cv.null_bitmap_bytes(), 2u);
    const u8* bm = cv.null_bitmap_data();
    EXPECT_EQ(bm[0], 0b10001001);
    EXPECT_EQ(bm[1], 0b00000001);
}

TEST(ColumnVectorTest, ReserveKeepsDataPointerStable) {
    auto cv = ColumnVector::empty(TypeId::INT32, false, 100);
    const byte* initial = cv.data();
    for (i32 i = 0; i < 100; ++i)
        cv.append_i32(i);
    EXPECT_EQ(cv.data(), initial);
    EXPECT_EQ(cv.size(), 100u);
}
