#include "database/database.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_database_test";

class DatabaseTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        auto r = Database::open(ROOT);
        ASSERT_TRUE(r.is_ok()) << r.error().message;
        db_ = std::make_unique<Database>(std::move(r.value()));

        auto cr = db_->execute("CREATE TABLE users (id INT, score DOUBLE)");
        ASSERT_TRUE(cr.is_ok()) << cr.error().message;
    }
    void TearDown() override { fs::remove_all(ROOT); }

    std::unique_ptr<Database> db_;
};

TEST_F(DatabaseTest, CreateTableOk) {
    auto r = db_->execute("CREATE TABLE items (item_id INT)");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().row_count(), 0u);
    EXPECT_EQ(r.value().rows_affected, 0u);
}

TEST_F(DatabaseTest, InsertReturnsRowCount) {
    auto r = db_->execute("INSERT INTO users VALUES (1, 1.0), (2, 2.0)");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().rows_affected, 2u);
}

TEST_F(DatabaseTest, SelectFromEmptyTable) {
    auto r = db_->execute("SELECT id FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().row_count(), 0u);
}

TEST_F(DatabaseTest, SelectSchemaCorrect) {
    auto r = db_->execute("SELECT id, score FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().schema.size(), 2u);
    EXPECT_EQ(r.value().schema[0].name, "id");
    EXPECT_EQ(r.value().schema[0].type, TypeId::INT32);
    EXPECT_EQ(r.value().schema[1].name, "score");
    EXPECT_EQ(r.value().schema[1].type, TypeId::DOUBLE);
}

TEST_F(DatabaseTest, InsertThenSelectRoundtrip) {
    ASSERT_TRUE(db_->execute("INSERT INTO users VALUES (1, 1.0), (2, 2.0), (3, 3.0)").is_ok());
    auto r = db_->execute("SELECT id FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().row_count(), 3u);
}

TEST_F(DatabaseTest, SelectValuesCorrect) {
    ASSERT_TRUE(db_->execute("INSERT INTO users VALUES (42, 3.14)").is_ok());
    auto r = db_->execute("SELECT id, score FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().row_count(), 1u);
    EXPECT_EQ(std::get<i32>(r.value().columns[0][0]), 42);
    EXPECT_DOUBLE_EQ(std::get<f64>(r.value().columns[1][0]), 3.14);
}

TEST_F(DatabaseTest, WhereFiltersRows) {
    ASSERT_TRUE(db_->execute("INSERT INTO users VALUES (1, 1.0), (2, 2.0), (3, 3.0)").is_ok());
    auto r = db_->execute("SELECT id FROM users WHERE id > 1");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().row_count(), 2u);
}

TEST_F(DatabaseTest, AggregateCountStar) {
    ASSERT_TRUE(db_->execute("INSERT INTO users VALUES (1, 1.0), (2, 2.0), (3, 3.0)").is_ok());
    auto r = db_->execute("SELECT count(*) FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().row_count(), 1u);
    EXPECT_EQ(std::get<i64>(r.value().columns[0][0]), 3);
}

TEST_F(DatabaseTest, SyntaxErrorPropagated) {
    auto r = db_->execute("SELEC broken sql");
    ASSERT_TRUE(r.is_err());
}

TEST_F(DatabaseTest, UnknownTableErrorPropagated) {
    auto r = db_->execute("SELECT id FROM ghost");
    ASSERT_TRUE(r.is_err());
}

TEST_F(DatabaseTest, OrderByAndLimit) {
    ASSERT_TRUE(db_->execute("INSERT INTO users VALUES (3, 3.0), (1, 1.0), (2, 2.0)").is_ok());
    auto r = db_->execute("SELECT id FROM users ORDER BY id ASC LIMIT 2");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().row_count(), 2u);
    EXPECT_EQ(std::get<i32>(r.value().columns[0][0]), 1);
    EXPECT_EQ(std::get<i32>(r.value().columns[0][1]), 2);
}
