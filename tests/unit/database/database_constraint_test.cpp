#include "database/database.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_constraint_test";

class ConstraintTest : public ::testing::Test {
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

    void exec_err(const std::string& sql) {
        auto r = db_->execute(sql);
        EXPECT_FALSE(r.is_ok()) << "expected error but query succeeded";
    }

    ExecuteResult exec_query(const std::string& sql) {
        auto r = db_->execute(sql);
        EXPECT_TRUE(r.is_ok()) << r.error().message;
        return std::move(r.value());
    }

    std::unique_ptr<Database> db_;
};

TEST_F(ConstraintTest, PrimaryKeyRejectsNull) {
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY, val INT)");
    exec_err("INSERT INTO t VALUES (NULL, 1)");
}

TEST_F(ConstraintTest, PrimaryKeyRejectsDuplicate) {
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY, val INT)");
    exec_ok("INSERT INTO t VALUES (1, 10)");
    exec_err("INSERT INTO t VALUES (1, 20)");
}

TEST_F(ConstraintTest, PrimaryKeyCompositeAllowsPartialMatch) {
    exec_ok("CREATE TABLE t (a INT NOT NULL, b INT NOT NULL, PRIMARY KEY (a, b))");
    exec_ok("INSERT INTO t VALUES (1, 1)");
    exec_ok("INSERT INTO t VALUES (1, 2)");
    exec_err("INSERT INTO t VALUES (1, 1)");
}

TEST_F(ConstraintTest, UniqueAllowsMultipleNulls) {
    exec_ok("CREATE TABLE t (id INT, val INT UNIQUE)");
    exec_ok("INSERT INTO t VALUES (1, NULL)");
    exec_ok("INSERT INTO t VALUES (2, NULL)");
}

TEST_F(ConstraintTest, UniqueRejectsDuplicate) {
    exec_ok("CREATE TABLE t (id INT, val INT UNIQUE)");
    exec_ok("INSERT INTO t VALUES (1, 42)");
    exec_err("INSERT INTO t VALUES (2, 42)");
}

TEST_F(ConstraintTest, UniqueTableLevel) {
    exec_ok("CREATE TABLE t (a INT NOT NULL, b INT NOT NULL, UNIQUE (a, b))");
    exec_ok("INSERT INTO t VALUES (1, 1)");
    exec_ok("INSERT INTO t VALUES (1, 2)");
    exec_err("INSERT INTO t VALUES (1, 1)");
}

TEST_F(ConstraintTest, DefaultApplied) {
    exec_ok("CREATE TABLE t (id INT, val INT DEFAULT 99)");
    exec_ok("INSERT INTO t (id) VALUES (1)");
    auto r = exec_query("SELECT val FROM t WHERE id = 1");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r.columns[0][0]), 99);
}

TEST_F(ConstraintTest, DefaultNullFallback) {
    exec_ok("CREATE TABLE t (id INT, val INT)");
    exec_ok("INSERT INTO t (id) VALUES (1)");
    auto r = exec_query("SELECT val FROM t WHERE id = 1");
    ASSERT_EQ(r.row_count(), 1u);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(r.columns[0][0]));
}

TEST_F(ConstraintTest, DefaultNotNullNoDefault) {
    exec_ok("CREATE TABLE t (id INT, val INT NOT NULL)");
    exec_err("INSERT INTO t (id) VALUES (1)");
}

TEST_F(ConstraintTest, ShowConstraints) {
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY, val INT UNIQUE)");
    auto r = exec_query("SHOW CONSTRAINTS FROM t");
    ASSERT_EQ(r.row_count(), 2u);
    bool found_pk = false, found_uq = false;
    for (size_t i = 0; i < 2; ++i) {
        auto kind = std::get<std::string>(r.columns[1][i]);
        if (kind == "PRIMARY KEY")
            found_pk = true;
        if (kind == "UNIQUE")
            found_uq = true;
    }
    EXPECT_TRUE(found_pk);
    EXPECT_TRUE(found_uq);
}

TEST_F(ConstraintTest, DropTableRemovesConstraints) {
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("DROP TABLE t");
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");
    exec_err("INSERT INTO t VALUES (1)");
}

TEST_F(ConstraintTest, BoolInPKRejected) {
    exec_err("CREATE TABLE t (flag BOOL PRIMARY KEY)");
}

TEST_F(ConstraintTest, DuplicatePKClause) {
    exec_err("CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)");
}

TEST_F(ConstraintTest, PKImpliesNotNull) {
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY)");
    exec_err("INSERT INTO t VALUES (NULL)");
}
