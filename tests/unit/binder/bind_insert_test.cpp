#include "binder/binder.h"
#include "catalog/catalog.h"
#include "frontend/runner.h"
#include "parser/lexer.h"
#include "parser/parser.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_bind_insert_test";

class BindInsertTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
        auto cat = Catalog::load(ROOT);
        ASSERT_TRUE(cat.is_ok());
        catalog_ = std::make_unique<Catalog>(std::move(cat.value()));

        Schema users{Column{"id", TypeId::INT32, false}, Column{"score", TypeId::DOUBLE, true},
                     Column{"ref", TypeId::INT64, true}};
        ASSERT_TRUE(catalog_->add_table("users", users).is_ok());
    }
    void TearDown() override { fs::remove_all(ROOT); }

    std::unique_ptr<Catalog> catalog_;
};

static ast::InsertStmt parse_insert(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    Parser p(src, std::move(toks.value()));
    auto stmts = p.parse();
    return std::move(std::get<ast::InsertStmt>(stmts.value()[0]));
}

TEST_F(BindInsertTest, ImplicitAllColumnsOK) {
    std::string src = "INSERT INTO users VALUES (1, 2.0, NULL)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().rows.size(), 1u);
    ASSERT_EQ(r.value().rows[0].size(), 3u);
}

TEST_F(BindInsertTest, ImplicitValuesInSchemaOrder) {
    std::string src = "INSERT INTO users VALUES (42, 3.14, NULL)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(std::get<i32>(r.value().rows[0][0]), 42);
    EXPECT_EQ(std::get<f64>(r.value().rows[0][1]), 3.14);
    EXPECT_TRUE(is_null(r.value().rows[0][2]));
}

TEST_F(BindInsertTest, ExplicitColumnListOK) {
    std::string src = "INSERT INTO users (id, score) VALUES (1, 2.0)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
}

TEST_F(BindInsertTest, ExplicitPermutationSchemaOrder) {
    std::string src = "INSERT INTO users (score, id) VALUES (9.9, 7)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(std::get<i32>(r.value().rows[0][0]), 7);
    EXPECT_EQ(std::get<f64>(r.value().rows[0][1]), 9.9);
}

TEST_F(BindInsertTest, MultipleRowsBound) {
    std::string src = "INSERT INTO users VALUES (1, 1.0, NULL), (2, 2.0, NULL)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().rows.size(), 2u);
}

TEST_F(BindInsertTest, ArityMismatchErrors) {
    std::string src = "INSERT INTO users VALUES (1, 2.0)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("arity mismatch"), std::string::npos);
}

TEST_F(BindInsertTest, TypeMismatchErrors) {
    std::string src = "INSERT INTO users VALUES (1.5, 2.0, NULL)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("type mismatch"), std::string::npos);
}

TEST_F(BindInsertTest, NullIntoNotNullErrors) {
    std::string src = "INSERT INTO users VALUES (NULL, 2.0, NULL)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("NOT NULL"), std::string::npos);
}

TEST_F(BindInsertTest, NullIntoNullableOK) {
    std::string src = "INSERT INTO users VALUES (1, NULL, NULL)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(is_null(r.value().rows[0][1]));
}

TEST_F(BindInsertTest, UnknownTableErrors) {
    std::string src = "INSERT INTO ghost VALUES (1)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown table"), std::string::npos);
}

TEST_F(BindInsertTest, UnknownColumnErrors) {
    std::string src = "INSERT INTO users (id, ghost) VALUES (1, 2)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown column"), std::string::npos);
}

TEST_F(BindInsertTest, DuplicateColumnErrors) {
    std::string src = "INSERT INTO users (id, id) VALUES (1, 2)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("duplicate column"), std::string::npos);
}

TEST_F(BindInsertTest, SmallIntIntoInt64ColumnOK) {
    std::string src = "INSERT INTO users VALUES (1, 2.0, 42)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto r = b.bind_insert(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(std::get<i64>(r.value().rows[0][2]), 42);
}

TEST_F(BindInsertTest, RunInsertRoundtrip) {
    std::string src = "INSERT INTO users VALUES (1, 1.0, NULL), (2, 2.0, NULL)";
    auto stmt = parse_insert(src);
    Binder b(*catalog_);
    auto bound = b.bind_insert(stmt, src);
    ASSERT_TRUE(bound.is_ok()) << bound.error().message;
    auto r = frontend::run_insert(*catalog_, bound.value());
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(catalog_->table("users")->row_count(), 2u);
}

TEST_F(BindInsertTest, BindDispatcherHandlesInsert) {
    std::string src = "INSERT INTO users VALUES (1, 1.0, NULL)";
    Lexer lex(src);
    auto toks = lex.tokenize();
    Parser p(src, std::move(toks.value()));
    auto stmts = p.parse();
    Binder b(*catalog_);
    auto r = b.bind(stmts.value()[0], src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(std::holds_alternative<bound::BoundInsert>(r.value()));
}
