#include "binder/binder.h"
#include "catalog/catalog.h"
#include "frontend/runner.h"
#include "parser/lexer.h"
#include "parser/parser.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_bind_create_table_test";

class BindCreateTableTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
        auto cat = Catalog::load(ROOT);
        ASSERT_TRUE(cat.is_ok());
        catalog_ = std::make_unique<Catalog>(std::move(cat.value()));
    }
    void TearDown() override { fs::remove_all(ROOT); }

    std::unique_ptr<Catalog> catalog_;
};

static ast::CreateTableStmt parse_create(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    Parser p(src, std::move(toks.value()));
    auto stmts = p.parse();
    return std::move(std::get<ast::CreateTableStmt>(stmts.value()[0]));
}

TEST_F(BindCreateTableTest, ValidSchemaBound) {
    std::string src = "CREATE TABLE users (id INT NOT NULL, name INT)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto r = b.bind_create_table(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
}

TEST_F(BindCreateTableTest, TableNamePreserved) {
    std::string src = "CREATE TABLE orders (id INT NOT NULL)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto r = b.bind_create_table(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().table_name, "orders");
}

TEST_F(BindCreateTableTest, ColumnNamesPreserved) {
    std::string src = "CREATE TABLE t (col_a INT NOT NULL, col_b INT)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto r = b.bind_create_table(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().schema.size(), 2u);
    EXPECT_EQ(r.value().schema[0].name, "col_a");
    EXPECT_EQ(r.value().schema[1].name, "col_b");
}

TEST_F(BindCreateTableTest, ColumnTypesPreserved) {
    std::string src = "CREATE TABLE t (a INT NOT NULL, b BIGINT NOT NULL, c DOUBLE NOT NULL)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto r = b.bind_create_table(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().schema[0].type, TypeId::INT32);
    EXPECT_EQ(r.value().schema[1].type, TypeId::INT64);
    EXPECT_EQ(r.value().schema[2].type, TypeId::DOUBLE);
}

TEST_F(BindCreateTableTest, ColumnNullabilityPreserved) {
    std::string src = "CREATE TABLE t (a INT NOT NULL, b INT)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto r = b.bind_create_table(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_FALSE(r.value().schema[0].nullable);
    EXPECT_TRUE(r.value().schema[1].nullable);
}

TEST_F(BindCreateTableTest, DuplicateColumnErrors) {
    std::string src = "CREATE TABLE t (id INT NOT NULL, id INT)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto r = b.bind_create_table(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("duplicate column"), std::string::npos);
}

TEST_F(BindCreateTableTest, RunnerAddsTableToCatalog) {
    std::string src = "CREATE TABLE employees (id INT NOT NULL)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto bound = b.bind_create_table(stmt, src);
    ASSERT_TRUE(bound.is_ok()) << bound.error().message;
    auto r = frontend::run_create_table(*catalog_, bound.value());
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(catalog_->has_table("employees"));
}

TEST_F(BindCreateTableTest, RunnerDuplicateTableErrors) {
    std::string src = "CREATE TABLE employees (id INT NOT NULL)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto bound = b.bind_create_table(stmt, src);
    ASSERT_TRUE(bound.is_ok());
    ASSERT_TRUE(frontend::run_create_table(*catalog_, bound.value()).is_ok());
    auto r = frontend::run_create_table(*catalog_, bound.value());
    ASSERT_TRUE(r.is_err());
}

TEST_F(BindCreateTableTest, RunnerSchemaRoundtrip) {
    std::string src = "CREATE TABLE products (id INT NOT NULL, price DOUBLE)";
    auto stmt = parse_create(src);
    Binder b(*catalog_);
    auto bound = b.bind_create_table(stmt, src);
    ASSERT_TRUE(bound.is_ok());
    ASSERT_TRUE(frontend::run_create_table(*catalog_, bound.value()).is_ok());
    const Schema* s = catalog_->schema_of("products");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->size(), 2u);
    EXPECT_EQ((*s)[0].name, "id");
    EXPECT_EQ((*s)[0].type, TypeId::INT32);
    EXPECT_EQ((*s)[1].name, "price");
    EXPECT_EQ((*s)[1].type, TypeId::DOUBLE);
}
