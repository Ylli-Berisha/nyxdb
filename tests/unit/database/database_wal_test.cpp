#include "database/database.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string WAL_DB = "/tmp/nyxdb_wal_e2e";

class DatabaseWalTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(WAL_DB); }
    void TearDown() override { fs::remove_all(WAL_DB); }
};

static Result<ExecuteResult> exec(Database& db, const std::string& sql) {
    return db.execute(sql);
}

TEST_F(DatabaseWalTest, CleanExitPreservesData) {
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        ASSERT_TRUE(exec(db, "CREATE TABLE t (id INT, v DOUBLE)").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO t VALUES (1, 1.5), (2, 2.5)").is_ok());
        ASSERT_TRUE(db.flush().is_ok());
    }
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        auto r = exec(db, "SELECT id, v FROM t");
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().row_count(), 2u);
    }
}

TEST_F(DatabaseWalTest, DirtyShutdownRecoversData) {
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        ASSERT_TRUE(exec(db, "CREATE TABLE t (id INT, name VARCHAR(30))").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO t VALUES (1, 'alice'), (2, 'bob')").is_ok());
        // no flush — simulates dirty shutdown
    }

    EXPECT_TRUE(fs::exists(WAL_DB + "/wal.bin"));

    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        auto r = exec(db, "SELECT id, name FROM t");
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().row_count(), 2u);
        EXPECT_EQ(std::get<i32>(r.value().columns[0][0]), 1);
        EXPECT_EQ(std::get<std::string>(r.value().columns[1][0]), "alice");
        EXPECT_EQ(std::get<i32>(r.value().columns[0][1]), 2);
        EXPECT_EQ(std::get<std::string>(r.value().columns[1][1]), "bob");
    }

    EXPECT_FALSE(fs::exists(WAL_DB + "/wal.bin"));
}

TEST_F(DatabaseWalTest, WalDeletedAfterCleanExit) {
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        ASSERT_TRUE(exec(db, "CREATE TABLE t (x INT)").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO t VALUES (42)").is_ok());
        ASSERT_TRUE(db.flush().is_ok());
    }
    EXPECT_FALSE(fs::exists(WAL_DB + "/wal.bin"));
}

TEST_F(DatabaseWalTest, MultipleTablesRecovery) {
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        ASSERT_TRUE(exec(db, "CREATE TABLE a (x INT)").is_ok());
        ASSERT_TRUE(exec(db, "CREATE TABLE b (y VARCHAR(20))").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO a VALUES (1), (2), (3)").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO b VALUES ('x'), ('y')").is_ok());
        // no flush
    }
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        auto ra = exec(db, "SELECT x FROM a");
        ASSERT_TRUE(ra.is_ok());
        EXPECT_EQ(ra.value().row_count(), 3u);
        auto rb = exec(db, "SELECT y FROM b");
        ASSERT_TRUE(rb.is_ok());
        EXPECT_EQ(rb.value().row_count(), 2u);
    }
}

TEST_F(DatabaseWalTest, MultipleInsertsRecovery) {
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        ASSERT_TRUE(exec(db, "CREATE TABLE t (id INT)").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO t VALUES (1), (2)").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO t VALUES (3), (4)").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO t VALUES (5)").is_ok());
        // no flush
    }
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        auto r = exec(db, "SELECT id FROM t");
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().row_count(), 5u);
        for (int i = 0; i < 5; ++i)
            EXPECT_EQ(std::get<i32>(r.value().columns[0][i]), i + 1);
    }
}

TEST_F(DatabaseWalTest, RecoveryIdempotent) {
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        ASSERT_TRUE(exec(db, "CREATE TABLE t (id INT)").is_ok());
        ASSERT_TRUE(exec(db, "INSERT INTO t VALUES (10), (20)").is_ok());
        // no flush
    }
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        auto r = exec(db, "SELECT id FROM t");
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().row_count(), 2u);
        ASSERT_TRUE(db.flush().is_ok());
    }
    {
        auto db_r = Database::open(WAL_DB);
        ASSERT_TRUE(db_r.is_ok());
        auto db = std::move(db_r.value());
        auto r = exec(db, "SELECT id FROM t");
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().row_count(), 2u);
    }
}
