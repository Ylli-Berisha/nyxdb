#include "storage/disk/column_page.h"

#include <gtest/gtest.h>

using namespace nyx;

TEST(ColumnPageTest, InitSetsHeaderFields) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, true);

    ColumnPage cp(p);
    EXPECT_EQ(cp.type(), TypeId::INT64);
    EXPECT_TRUE(cp.nullable());
    EXPECT_EQ(cp.value_count(), 0u);
    EXPECT_GT(cp.capacity(), 0u);
    EXPECT_FALSE(cp.is_full());
}

TEST(ColumnPageTest, AppendGetInt64) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);

    for (i64 v = 0; v < 100; ++v)
        ASSERT_TRUE(cp.append_i64(v).is_ok());

    EXPECT_EQ(cp.value_count(), 100u);
    for (u16 i = 0; i < 100; ++i) {
        auto r = cp.get_i64(i);
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value(), static_cast<i64>(i));
    }
}

TEST(ColumnPageTest, AppendGetInt32) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT32, false);
    ColumnPage cp(p);

    for (i32 v = 0; v < 50; ++v)
        ASSERT_TRUE(cp.append_i32(v * 2).is_ok());

    for (u16 i = 0; i < 50; ++i) {
        auto r = cp.get_i32(i);
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value(), static_cast<i32>(i * 2));
    }
}

TEST(ColumnPageTest, AppendGetDouble) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::DOUBLE, false);
    ColumnPage cp(p);

    ASSERT_TRUE(cp.append_f64(3.14).is_ok());
    ASSERT_TRUE(cp.append_f64(-2.71).is_ok());

    EXPECT_DOUBLE_EQ(cp.get_f64(0).value(), 3.14);
    EXPECT_DOUBLE_EQ(cp.get_f64(1).value(), -2.71);
}

TEST(ColumnPageTest, NullBitmapWorks) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, true);
    ColumnPage cp(p);

    ASSERT_TRUE(cp.append_i64(10).is_ok());
    ASSERT_TRUE(cp.append_null().is_ok());
    ASSERT_TRUE(cp.append_i64(30).is_ok());
    ASSERT_TRUE(cp.append_null().is_ok());

    EXPECT_FALSE(cp.is_null(0));
    EXPECT_TRUE(cp.is_null(1));
    EXPECT_FALSE(cp.is_null(2));
    EXPECT_TRUE(cp.is_null(3));

    EXPECT_EQ(cp.get_i64(0).value(), 10);
    EXPECT_EQ(cp.get_i64(2).value(), 30);
    EXPECT_TRUE(cp.get_i64(1).is_err()); // reading null errors
}

TEST(ColumnPageTest, AppendNullOnNotNullableErrors) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);

    auto res = cp.append_null();
    EXPECT_TRUE(res.is_err());
}

TEST(ColumnPageTest, TypeMismatchOnAppendErrors) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);

    EXPECT_TRUE(cp.append_i32(5).is_err());
    EXPECT_TRUE(cp.append_f64(5.0).is_err());
}

TEST(ColumnPageTest, TypeMismatchOnGetErrors) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);
    ASSERT_TRUE(cp.append_i64(42).is_ok());

    EXPECT_TRUE(cp.get_i32(0).is_err());
    EXPECT_TRUE(cp.get_f64(0).is_err());
}

TEST(ColumnPageTest, GetOutOfRangeErrors) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);
    ASSERT_TRUE(cp.append_i64(1).is_ok());

    EXPECT_TRUE(cp.get_i64(5).is_err());
}

TEST(ColumnPageTest, IsFullAtCapacity) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);

    for (u16 i = 0; i < cp.capacity(); ++i)
        ASSERT_TRUE(cp.append_i64(i).is_ok());

    EXPECT_TRUE(cp.is_full());
    EXPECT_TRUE(cp.append_i64(0).is_err());
}

TEST(ColumnPageTest, MinMaxUpdatesOnAppendInt64) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);

    for (i64 v : {5, 3, 8, 1, 6})
        ASSERT_TRUE(cp.append_i64(v).is_ok());

    ASSERT_TRUE(cp.min_i64().has_value());
    ASSERT_TRUE(cp.max_i64().has_value());
    EXPECT_EQ(*cp.min_i64(), 1);
    EXPECT_EQ(*cp.max_i64(), 8);
}

TEST(ColumnPageTest, MinMaxAcrossTypes) {
    {
        Page p{};
        p.reset(1);
        ColumnPage::init(p, TypeId::INT32, false);
        ColumnPage cp(p);
        for (i32 v : {5, 3, 8, 1, 6})
            ASSERT_TRUE(cp.append_i32(v).is_ok());
        EXPECT_EQ(*cp.min_i32(), 1);
        EXPECT_EQ(*cp.max_i32(), 8);
    }
    {
        Page p{};
        p.reset(1);
        ColumnPage::init(p, TypeId::DOUBLE, false);
        ColumnPage cp(p);
        for (f64 v : {5.0, 3.5, 8.25, 1.75, 6.0})
            ASSERT_TRUE(cp.append_f64(v).is_ok());
        EXPECT_DOUBLE_EQ(*cp.min_f64(), 1.75);
        EXPECT_DOUBLE_EQ(*cp.max_f64(), 8.25);
    }
}

TEST(ColumnPageTest, MinMaxEmptyOnFreshPage) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);
    EXPECT_FALSE(cp.min_i64().has_value());
    EXPECT_FALSE(cp.max_i64().has_value());
}

TEST(ColumnPageTest, MinMaxEmptyOnAllNullPage) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, true);
    ColumnPage cp(p);
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(cp.append_null().is_ok());

    EXPECT_FALSE(cp.min_i64().has_value());
    EXPECT_FALSE(cp.max_i64().has_value());
    EXPECT_EQ(cp.null_count(), 3u);
    EXPECT_TRUE(cp.has_nulls());
}

TEST(ColumnPageTest, MinMaxTypeMismatchReturnsNullopt) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, false);
    ColumnPage cp(p);
    ASSERT_TRUE(cp.append_i64(42).is_ok());

    EXPECT_FALSE(cp.min_i32().has_value());
    EXPECT_FALSE(cp.max_f64().has_value());
}

TEST(ColumnPageTest, NullCountAndFlag) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, true);
    ColumnPage cp(p);

    EXPECT_FALSE(cp.has_nulls());
    EXPECT_EQ(cp.null_count(), 0u);

    ASSERT_TRUE(cp.append_null().is_ok());
    EXPECT_TRUE(cp.has_nulls());
    EXPECT_EQ(cp.null_count(), 1u);

    ASSERT_TRUE(cp.append_i64(7).is_ok());
    ASSERT_TRUE(cp.append_null().is_ok());
    EXPECT_EQ(cp.null_count(), 2u);
    EXPECT_TRUE(cp.has_nulls());
}

TEST(ColumnPageTest, MinMaxIgnoresNulls) {
    Page p{};
    p.reset(1);
    ColumnPage::init(p, TypeId::INT64, true);
    ColumnPage cp(p);

    ASSERT_TRUE(cp.append_null().is_ok());
    ASSERT_TRUE(cp.append_i64(10).is_ok());
    ASSERT_TRUE(cp.append_null().is_ok());
    ASSERT_TRUE(cp.append_i64(-5).is_ok());
    ASSERT_TRUE(cp.append_null().is_ok());
    ASSERT_TRUE(cp.append_i64(42).is_ok());

    EXPECT_EQ(*cp.min_i64(), -5);
    EXPECT_EQ(*cp.max_i64(), 42);
    EXPECT_EQ(cp.null_count(), 3u);
}
