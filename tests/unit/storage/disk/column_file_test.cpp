#include "storage/disk/column_file.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_FILE = "/tmp/nyxdb_cf_test.col";

class ColumnFileTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove(TEST_FILE); }
    void TearDown() override { fs::remove(TEST_FILE); }
};

TEST_F(ColumnFileTest, CreateInitsFirstPage) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());
    EXPECT_EQ(cf.row_count(), 0u);
    EXPECT_EQ(cf.type(), TypeId::INT64);
    EXPECT_FALSE(cf.nullable());
}

TEST_F(ColumnFileTest, AppendAndGetRoundTrip) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    for (i64 v = 0; v < 500; ++v)
        ASSERT_TRUE(cf.append_i64(v).is_ok());

    EXPECT_EQ(cf.row_count(), 500u);
    for (u64 i = 0; i < 500; ++i) {
        auto g = cf.get_i64(i);
        ASSERT_TRUE(g.is_ok()) << "row " << i;
        EXPECT_EQ(g.value(), static_cast<i64>(i));
    }
}

TEST_F(ColumnFileTest, AppendCrossesPageBoundary) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    u16 cap = cf.page_capacity();
    u64 total = cap * 2 + 5;
    for (u64 i = 0; i < total; ++i)
        ASSERT_TRUE(cf.append_i64(static_cast<i64>(i)).is_ok());

    EXPECT_EQ(cf.row_count(), total);
    EXPECT_EQ(cf.get_i64(0).value(), 0);
    EXPECT_EQ(cf.get_i64(cap - 1).value(), static_cast<i64>(cap - 1));
    EXPECT_EQ(cf.get_i64(cap).value(), static_cast<i64>(cap));
    EXPECT_EQ(cf.get_i64(cap * 2).value(), static_cast<i64>(cap * 2));
    EXPECT_EQ(cf.get_i64(total - 1).value(), static_cast<i64>(total - 1));
}

TEST_F(ColumnFileTest, NullsWithBitmap) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, true);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    ASSERT_TRUE(cf.append_i64(1).is_ok());
    ASSERT_TRUE(cf.append_null().is_ok());
    ASSERT_TRUE(cf.append_i64(3).is_ok());

    EXPECT_FALSE(cf.is_null(0));
    EXPECT_TRUE(cf.is_null(1));
    EXPECT_FALSE(cf.is_null(2));
    EXPECT_EQ(cf.get_i64(0).value(), 1);
    EXPECT_TRUE(cf.get_i64(1).is_err());
    EXPECT_EQ(cf.get_i64(2).value(), 3);
}

TEST_F(ColumnFileTest, PersistenceAcrossReopens) {
    u16 cap = 0;
    {
        auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
        ASSERT_TRUE(res.is_ok());
        auto cf = std::move(res.value());
        cap = cf.page_capacity();

        for (i64 v = 0; v < 200; ++v)
            ASSERT_TRUE(cf.append_i64(v * 3).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
        ASSERT_TRUE(cf.fsync().is_ok());
    }

    auto open_res = ColumnFile::open(TEST_FILE);
    ASSERT_TRUE(open_res.is_ok());
    auto cf2 = std::move(open_res.value());
    EXPECT_EQ(cf2.type(), TypeId::INT64);
    EXPECT_FALSE(cf2.nullable());
    EXPECT_EQ(cf2.page_capacity(), cap);
    EXPECT_EQ(cf2.row_count(), 200u);
    for (u64 i = 0; i < 200; ++i)
        EXPECT_EQ(cf2.get_i64(i).value(), static_cast<i64>(i * 3));
}

TEST_F(ColumnFileTest, ScanIteratesAllPages) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    u16 cap = cf.page_capacity();
    u64 total = cap * 2 + 3;
    for (u64 i = 0; i < total; ++i)
        ASSERT_TRUE(cf.append_i64(static_cast<i64>(i)).is_ok());

    u64 seen = 0;
    i64 running_value = 0;
    auto res_scan = cf.scan([&](const ColumnPage& cp) {
        for (u16 i = 0; i < cp.value_count(); ++i) {
            auto v = cp.get_i64(i);
            ASSERT_TRUE(v.is_ok());
            EXPECT_EQ(v.value(), running_value);
            ++running_value;
            ++seen;
        }
    });
    ASSERT_TRUE(res_scan.is_ok());
    EXPECT_EQ(seen, total);
}

TEST_F(ColumnFileTest, CreateFailsOnExistingFile) {
    ASSERT_TRUE(ColumnFile::create(TEST_FILE, TypeId::INT64, false).is_ok());
    auto second = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    EXPECT_TRUE(second.is_err());
}

TEST_F(ColumnFileTest, OpenFailsOnMissingFile) {
    fs::remove(TEST_FILE);
    auto res = ColumnFile::open(TEST_FILE);
    EXPECT_TRUE(res.is_err());
}

TEST_F(ColumnFileTest, TypeMismatchOnAppendErrors) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    EXPECT_TRUE(cf.append_i32(5).is_err());
    EXPECT_TRUE(cf.append_f64(5.0).is_err());
}

TEST_F(ColumnFileTest, AppendBulkAcrossPages) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    u16 cap = cf.page_capacity();
    u64 total = cap * 2 + 3;

    std::vector<Value> batch;
    batch.reserve(total);
    for (u64 i = 0; i < total; ++i)
        batch.emplace_back(static_cast<i64>(i * 7));

    ASSERT_TRUE(cf.append_bulk(batch).is_ok());
    EXPECT_EQ(cf.row_count(), total);
    EXPECT_EQ(cf.get_i64(0).value(), 0);
    EXPECT_EQ(cf.get_i64(cap - 1).value(), static_cast<i64>((cap - 1) * 7));
    EXPECT_EQ(cf.get_i64(cap).value(), static_cast<i64>(cap * 7));
    EXPECT_EQ(cf.get_i64(total - 1).value(), static_cast<i64>((total - 1) * 7));
}

TEST_F(ColumnFileTest, AppendBulkWithNulls) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, true);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    std::vector<Value> batch;
    for (i64 i = 0; i < 20; ++i) {
        if (i % 2 == 0)
            batch.emplace_back(i);
        else
            batch.emplace_back(std::monostate{});
    }

    ASSERT_TRUE(cf.append_bulk(batch).is_ok());
    EXPECT_EQ(cf.row_count(), 20u);
    for (u64 i = 0; i < 20; ++i) {
        if (i % 2 == 0) {
            EXPECT_FALSE(cf.is_null(i));
            EXPECT_EQ(cf.get_i64(i).value(), static_cast<i64>(i));
        } else {
            EXPECT_TRUE(cf.is_null(i));
        }
    }
}

TEST_F(ColumnFileTest, AppendBulkTypeMismatchErrors) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    std::vector<Value> batch = {static_cast<i64>(1), static_cast<i32>(2), static_cast<i64>(3)};
    EXPECT_TRUE(cf.append_bulk(batch).is_err());
}

TEST_F(ColumnFileTest, AppendBulkEmpty) {
    auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
    ASSERT_TRUE(res.is_ok());
    auto cf = std::move(res.value());

    std::vector<Value> batch;
    ASSERT_TRUE(cf.append_bulk(batch).is_ok());
    EXPECT_EQ(cf.row_count(), 0u);
}

TEST_F(ColumnFileTest, RotateDoesNotDoubleWrite) {
    // Insert enough values to force at least one rotation, then verify all
    // pages round-trip through DiskManager. Regression guard: if rotate's
    // reserve_page_id caused a hole, this fails on reopen.
    {
        auto res = ColumnFile::create(TEST_FILE, TypeId::INT64, false);
        ASSERT_TRUE(res.is_ok());
        auto cf = std::move(res.value());

        u64 total = cf.page_capacity() * 3 + 5;
        for (u64 i = 0; i < total; ++i)
            ASSERT_TRUE(cf.append_i64(static_cast<i64>(i)).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
        ASSERT_TRUE(cf.fsync().is_ok());
    }

    auto reopen = ColumnFile::open(TEST_FILE);
    ASSERT_TRUE(reopen.is_ok());
    auto cf = std::move(reopen.value());
    for (u64 i = 0; i < cf.row_count(); ++i)
        EXPECT_EQ(cf.get_i64(i).value(), static_cast<i64>(i));
}
