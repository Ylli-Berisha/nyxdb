#include "database/database.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_index_test";

class IndexTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        auto r = Database::open(ROOT);
        ASSERT_TRUE(r.is_ok()) << r.error().message;
        db_ = std::make_unique<Database>(std::move(r.value()));
    }
    void TearDown() override { fs::remove_all(ROOT); }

    void exec_ok(const std::string& sql) {
        auto r = db_->execute(sql);
        ASSERT_TRUE(r.is_ok()) << r.error().message;
    }

    ExecuteResult exec_query(const std::string& sql) {
        auto r = db_->execute(sql);
        EXPECT_TRUE(r.is_ok()) << r.error().message;
        return std::move(r.value());
    }

    std::unique_ptr<Database> db_;
};

TEST_F(IndexTest, CreateIndexPointLookup) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR(20))");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row" + std::to_string(i) + "')");
    }
    auto r = exec_query("SELECT id FROM t WHERE id = 42");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r.columns[0][0]), 42);
}

TEST_F(IndexTest, CreateIndexRangeScan) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    for (int i = 1; i <= 100; ++i)
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    auto r = exec_query("SELECT id FROM t WHERE id > 50");
    EXPECT_EQ(r.row_count(), 50u);
}

TEST_F(IndexTest, NullInIndex) {
    exec_ok("CREATE TABLE t (id INT, val INT)");
    exec_ok("CREATE INDEX idx_val ON t (val)");
    exec_ok("INSERT INTO t VALUES (1, 10)");
    exec_ok("INSERT INTO t VALUES (2, 20)");
    exec_ok("INSERT INTO t VALUES (3, NULL)");
    auto r = exec_query("SELECT id FROM t WHERE val = 10");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r.columns[0][0]), 1);
}

TEST_F(IndexTest, NonUnique) {
    exec_ok("CREATE TABLE t (id INT, v INT)");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    exec_ok("INSERT INTO t VALUES (5, 1)");
    exec_ok("INSERT INTO t VALUES (5, 2)");
    exec_ok("INSERT INTO t VALUES (5, 3)");
    auto r = exec_query("SELECT id FROM t WHERE id = 5");
    EXPECT_EQ(r.row_count(), 3u);
}

TEST_F(IndexTest, UniqueIndexRejects) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("CREATE UNIQUE INDEX uidx ON t (id)");
    exec_ok("INSERT INTO t VALUES (1)");
    auto r = db_->execute("INSERT INTO t VALUES (1)");
    EXPECT_TRUE(r.is_err());
    auto count = exec_query("SELECT id FROM t WHERE id = 1");
    EXPECT_EQ(count.row_count(), 1u);
}

TEST_F(IndexTest, InsertMaintainsIndex) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1), (2), (3)");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    exec_ok("INSERT INTO t VALUES (99)");
    auto r = exec_query("SELECT id FROM t WHERE id = 99");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r.columns[0][0]), 99);
}

TEST_F(IndexTest, DeleteHiddenByBitmap) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    exec_ok("INSERT INTO t VALUES (1), (2), (3)");
    exec_ok("DELETE FROM t WHERE id = 2");
    auto r = exec_query("SELECT id FROM t WHERE id = 2");
    EXPECT_EQ(r.row_count(), 0u);
}

TEST_F(IndexTest, UpdateVisible) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("UPDATE t SET id = 99 WHERE id = 1");
    auto r1 = exec_query("SELECT id FROM t WHERE id = 1");
    EXPECT_EQ(r1.row_count(), 0u);
    auto r2 = exec_query("SELECT id FROM t WHERE id = 99");
    ASSERT_EQ(r2.row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r2.columns[0][0]), 99);
}

TEST_F(IndexTest, PersistenceAfterReopen) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    for (int i = 1; i <= 10; ++i)
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    ASSERT_TRUE(db_->flush().is_ok());
    db_.reset();

    auto r2 = Database::open(ROOT);
    ASSERT_TRUE(r2.is_ok()) << r2.error().message;
    db_ = std::make_unique<Database>(std::move(r2.value()));

    auto r = exec_query("SELECT id FROM t WHERE id = 7");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r.columns[0][0]), 7);
}

TEST_F(IndexTest, DropIndex) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("CREATE INDEX idx_id ON t (id)");
    exec_ok("INSERT INTO t VALUES (1), (2), (3)");
    exec_ok("DROP INDEX idx_id ON t");
    auto r = exec_query("SELECT id FROM t WHERE id = 2");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r.columns[0][0]), 2);
}

TEST_F(IndexTest, ShowIndexes) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR(20))");
    exec_ok("CREATE UNIQUE INDEX uidx ON t (id)");
    auto r = exec_query("SHOW INDEXES FROM t");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_EQ(std::get<std::string>(r.columns[0][0]), "uidx");
    EXPECT_EQ(std::get<std::string>(r.columns[1][0]), "id");
    EXPECT_EQ(std::get<bool>(r.columns[2][0]), true);
}

TEST_F(IndexTest, BoolIndexRejected) {
    exec_ok("CREATE TABLE t (id INT, flag BOOL)");
    auto r = db_->execute("CREATE INDEX idx_flag ON t (flag)");
    EXPECT_TRUE(r.is_err());
}

TEST_F(IndexTest, CompositeIndex) {
    exec_ok("CREATE TABLE t (a INT, b INT, v INT)");
    exec_ok("CREATE INDEX idx_ab ON t (a, b)");
    exec_ok("INSERT INTO t VALUES (1, 10, 100)");
    exec_ok("INSERT INTO t VALUES (1, 20, 200)");
    exec_ok("INSERT INTO t VALUES (2, 10, 300)");
    auto show = exec_query("SHOW INDEXES FROM t");
    ASSERT_EQ(show.row_count(), 1u);
    EXPECT_EQ(std::get<std::string>(show.columns[0][0]), "idx_ab");
    EXPECT_EQ(std::get<std::string>(show.columns[1][0]), "a, b");
    auto r = exec_query("SELECT v FROM t WHERE a = 1");
    EXPECT_EQ(r.row_count(), 2u);
}
