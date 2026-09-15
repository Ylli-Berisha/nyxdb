#include "storage/disk/column_file.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string CF_FILE = "/tmp/nyxdb_truncate_cf.col";
static const std::string TABLE_DIR = "/tmp/nyxdb_truncate_table";

class ColumnFileTruncateTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove(CF_FILE); }
    void TearDown() override { fs::remove(CF_FILE); }
};

TEST_F(ColumnFileTruncateTest, TruncateToZero) {
    {
        auto r = ColumnFile::create(CF_FILE, TypeId::INT32, false);
        ASSERT_TRUE(r.is_ok());
        auto cf = std::move(r.value());
        for (i32 i = 0; i < 50; ++i)
            ASSERT_TRUE(cf.append_i32(i).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
    }
    auto r2 = ColumnFile::open(CF_FILE);
    ASSERT_TRUE(r2.is_ok());
    auto cf2 = std::move(r2.value());
    ASSERT_TRUE(cf2.truncate(0).is_ok());
    EXPECT_EQ(cf2.row_count(), 0u);
}

TEST_F(ColumnFileTruncateTest, TruncateWithinFirstPage) {
    {
        auto r = ColumnFile::create(CF_FILE, TypeId::INT32, false);
        ASSERT_TRUE(r.is_ok());
        auto cf = std::move(r.value());
        for (i32 i = 0; i < 200; ++i)
            ASSERT_TRUE(cf.append_i32(i).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
    }
    auto r2 = ColumnFile::open(CF_FILE);
    ASSERT_TRUE(r2.is_ok());
    auto cf2 = std::move(r2.value());
    ASSERT_TRUE(cf2.truncate(100).is_ok());
    EXPECT_EQ(cf2.row_count(), 100u);
    for (u64 i = 0; i < 100; ++i)
        EXPECT_EQ(cf2.get_i32(i).value(), static_cast<i32>(i));
}

TEST_F(ColumnFileTruncateTest, TruncateAcrossPageBoundary) {
    u64 cap;
    {
        auto r = ColumnFile::create(CF_FILE, TypeId::INT32, false);
        ASSERT_TRUE(r.is_ok());
        auto cf = std::move(r.value());
        cap = cf.page_capacity();
        for (u64 i = 0; i < cap * 3; ++i)
            ASSERT_TRUE(cf.append_i32(static_cast<i32>(i)).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
    }
    auto r2 = ColumnFile::open(CF_FILE);
    ASSERT_TRUE(r2.is_ok());
    auto cf2 = std::move(r2.value());
    u64 target = cap + cap / 2;
    ASSERT_TRUE(cf2.truncate(target).is_ok());
    EXPECT_EQ(cf2.row_count(), target);
    for (u64 i = 0; i < target; ++i)
        EXPECT_EQ(cf2.get_i32(i).value(), static_cast<i32>(i));
}

TEST_F(ColumnFileTruncateTest, TruncateToExactPageBoundary) {
    u64 cap;
    {
        auto r = ColumnFile::create(CF_FILE, TypeId::INT32, false);
        ASSERT_TRUE(r.is_ok());
        auto cf = std::move(r.value());
        cap = cf.page_capacity();
        for (u64 i = 0; i < cap * 2; ++i)
            ASSERT_TRUE(cf.append_i32(static_cast<i32>(i)).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
    }
    auto r2 = ColumnFile::open(CF_FILE);
    ASSERT_TRUE(r2.is_ok());
    auto cf2 = std::move(r2.value());
    ASSERT_TRUE(cf2.truncate(cap).is_ok());
    EXPECT_EQ(cf2.row_count(), cap);
    for (u64 i = 0; i < cap; ++i)
        EXPECT_EQ(cf2.get_i32(i).value(), static_cast<i32>(i));
}

TEST_F(ColumnFileTruncateTest, AppendAfterTruncate) {
    {
        auto r = ColumnFile::create(CF_FILE, TypeId::INT32, false);
        ASSERT_TRUE(r.is_ok());
        auto cf = std::move(r.value());
        for (i32 i = 0; i < 100; ++i)
            ASSERT_TRUE(cf.append_i32(i).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
    }
    auto r2 = ColumnFile::open(CF_FILE);
    ASSERT_TRUE(r2.is_ok());
    auto cf2 = std::move(r2.value());
    ASSERT_TRUE(cf2.truncate(50).is_ok());
    ASSERT_TRUE(cf2.append_i32(999).is_ok());
    EXPECT_EQ(cf2.row_count(), 51u);
    EXPECT_EQ(cf2.get_i32(50).value(), 999);
}

TEST_F(ColumnFileTruncateTest, TruncateVarchar) {
    {
        auto r = ColumnFile::create(CF_FILE, TypeId::VARCHAR, false, 20);
        ASSERT_TRUE(r.is_ok());
        auto cf = std::move(r.value());
        for (int i = 0; i < 50; ++i)
            ASSERT_TRUE(cf.append_str("str" + std::to_string(i)).is_ok());
        ASSERT_TRUE(cf.flush().is_ok());
    }
    auto r2 = ColumnFile::open(CF_FILE);
    ASSERT_TRUE(r2.is_ok());
    auto cf2 = std::move(r2.value());
    ASSERT_TRUE(cf2.truncate(25).is_ok());
    EXPECT_EQ(cf2.row_count(), 25u);
    EXPECT_EQ(cf2.get_str(0), "str0");
    EXPECT_EQ(cf2.get_str(24), "str24");
}

class TableTruncateTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(TABLE_DIR); }
    void TearDown() override { fs::remove_all(TABLE_DIR); }
};

TEST_F(TableTruncateTest, TruncateAllColumns) {
    {
        Schema schema = {{"id", TypeId::INT32, false, 0}, {"name", TypeId::VARCHAR, false, 20}};
        auto r = Table::create(TABLE_DIR, "t", schema);
        ASSERT_TRUE(r.is_ok());
        auto t = std::move(r.value());
        for (i32 i = 0; i < 100; ++i)
            ASSERT_TRUE(t.insert_many({{{i}, {std::string("n") + std::to_string(i)}}}).is_ok());
        ASSERT_TRUE(t.flush().is_ok());
    }
    auto r2 = Table::open(TABLE_DIR, "t");
    ASSERT_TRUE(r2.is_ok());
    auto t2 = std::move(r2.value());
    ASSERT_TRUE(t2.truncate(50).is_ok());
    EXPECT_EQ(t2.row_count(), 50u);
}
