#include "database/database.h"
#include "storage/vacuum.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_vacuum_test";

class VacuumTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(ROOT); }
    void TearDown() override { fs::remove_all(ROOT); }
};

static i64 count(Database& db, const std::string& tbl) {
    auto r = db.execute("SELECT id FROM " + tbl);
    if (!r.is_ok() || r.value().columns.empty())
        return -1;
    return static_cast<i64>(r.value().columns[0].size());
}

TEST_F(VacuumTest, ManualVacuumRemovesDeadRows) {
    auto db_r = Database::open(ROOT);
    ASSERT_TRUE(db_r.is_ok());
    auto db = std::move(db_r.value());

    ASSERT_TRUE(db.execute("CREATE TABLE t (id INT NOT NULL)").is_ok());
    for (int i = 0; i < 100; ++i)
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ")").is_ok());

    ASSERT_TRUE(db.execute("DELETE FROM t WHERE id >= 20 AND id < 45").is_ok());

    auto r = db.execute("VACUUM t");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().rows_affected, 25u);

    EXPECT_EQ(count(db, "t"), 75);
}

TEST_F(VacuumTest, VacuumNoopWhenNoDeletions) {
    auto db_r = Database::open(ROOT);
    ASSERT_TRUE(db_r.is_ok());
    auto db = std::move(db_r.value());

    ASSERT_TRUE(db.execute("CREATE TABLE t (id INT NOT NULL)").is_ok());
    for (int i = 0; i < 50; ++i)
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ")").is_ok());

    auto r = db.execute("VACUUM t");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().rows_affected, 0u);
    EXPECT_EQ(count(db, "t"), 50);
}

TEST_F(VacuumTest, VacuumStatsZeroAfterVacuum) {
    auto db_r = Database::open(ROOT);
    ASSERT_TRUE(db_r.is_ok());
    auto db = std::move(db_r.value());

    ASSERT_TRUE(db.execute("CREATE TABLE t (id INT NOT NULL)").is_ok());
    for (int i = 0; i < 80; ++i)
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ")").is_ok());

    ASSERT_TRUE(db.execute("DELETE FROM t WHERE id < 30").is_ok());
    ASSERT_TRUE(db.execute("VACUUM t").is_ok());

    auto cat_r = Catalog::load(ROOT);
    ASSERT_TRUE(cat_r.is_ok());
    Table* tbl = cat_r.value().table("t");
    if (tbl) {
        auto stats = table_dead_row_stats(tbl);
        EXPECT_EQ(stats.dead_rows, 0u);
        EXPECT_EQ(stats.total_rows, 50u);
    }
}

TEST_F(VacuumTest, AutoTriggerAboveThreshold) {
    auto db_r = Database::open(ROOT);
    ASSERT_TRUE(db_r.is_ok());
    auto db = std::move(db_r.value());

    ASSERT_TRUE(db.execute("CREATE TABLE t (id INT NOT NULL)").is_ok());
    for (int i = 0; i < 100; ++i)
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ")").is_ok());

    ASSERT_TRUE(db.execute("DELETE FROM t WHERE id >= 25").is_ok());

    std::this_thread::sleep_for(std::chrono::seconds(6));

    EXPECT_EQ(count(db, "t"), 25);

    auto noop_r = db.execute("VACUUM t");
    ASSERT_TRUE(noop_r.is_ok());
    EXPECT_EQ(noop_r.value().rows_affected, 0u);
}
