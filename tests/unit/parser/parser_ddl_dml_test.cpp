#include "parser/lexer.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

using namespace nyx;

static ast::Statement parse_stmt(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    EXPECT_TRUE(toks.is_ok()) << toks.error().message;
    Parser p(src, std::move(toks.value()));
    auto r = p.parse_statement();
    EXPECT_TRUE(r.is_ok()) << r.error().message;
    return std::move(r.value());
}

static Result<ast::Statement> try_parse_statement(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    if (toks.is_err())
        return Result<ast::Statement>::err(toks.error().message);
    Parser p(src, std::move(toks.value()));
    return p.parse_statement();
}

template <typename T> static const T& as(const ast::Expr& e) {
    return std::get<T>(e.node);
}

template <typename T> static bool is(const ast::Expr& e) {
    return std::holds_alternative<T>(e.node);
}

TEST(ParserDdlDmlTest, CreateTableSingleIntCol) {
    auto s = parse_stmt("CREATE TABLE t (id INT)");
    ASSERT_TRUE(std::holds_alternative<ast::CreateTableStmt>(s));
    const auto& c = std::get<ast::CreateTableStmt>(s);
    EXPECT_EQ(c.table_name, "t");
    ASSERT_EQ(c.columns.size(), 1u);
    EXPECT_EQ(c.columns[0].name, "id");
    EXPECT_EQ(c.columns[0].type, TypeId::INT32);
    EXPECT_TRUE(c.columns[0].nullable);
}

TEST(ParserDdlDmlTest, CreateTableAllTypes) {
    auto s = parse_stmt("CREATE TABLE t (a INT, b INTEGER, c BIGINT, d DOUBLE)");
    const auto& c = std::get<ast::CreateTableStmt>(s);
    ASSERT_EQ(c.columns.size(), 4u);
    EXPECT_EQ(c.columns[0].type, TypeId::INT32);
    EXPECT_EQ(c.columns[1].type, TypeId::INT32);
    EXPECT_EQ(c.columns[2].type, TypeId::INT64);
    EXPECT_EQ(c.columns[3].type, TypeId::DOUBLE);
}

TEST(ParserDdlDmlTest, CreateTableNotNull) {
    auto s = parse_stmt("CREATE TABLE t (a INT NOT NULL, b DOUBLE)");
    const auto& c = std::get<ast::CreateTableStmt>(s);
    ASSERT_EQ(c.columns.size(), 2u);
    EXPECT_FALSE(c.columns[0].nullable);
    EXPECT_TRUE(c.columns[1].nullable);
}

TEST(ParserDdlDmlTest, CreateTableKeywordsCaseInsensitive) {
    auto s = parse_stmt("create table T (X int not null)");
    const auto& c = std::get<ast::CreateTableStmt>(s);
    EXPECT_EQ(c.table_name, "t");
    EXPECT_EQ(c.columns[0].name, "x");
    EXPECT_FALSE(c.columns[0].nullable);
}

TEST(ParserDdlDmlTest, InsertAllColumnsSingleRow) {
    auto s = parse_stmt("INSERT INTO t VALUES (1, 2.5)");
    ASSERT_TRUE(std::holds_alternative<ast::InsertStmt>(s));
    const auto& i = std::get<ast::InsertStmt>(s);
    EXPECT_EQ(i.table_name, "t");
    EXPECT_EQ(i.columns.size(), 0u);
    ASSERT_EQ(i.rows.size(), 1u);
    ASSERT_EQ(i.rows[0].size(), 2u);
    EXPECT_TRUE(is<ast::IntLit>(*i.rows[0][0]));
    EXPECT_TRUE(is<ast::DoubleLit>(*i.rows[0][1]));
}

TEST(ParserDdlDmlTest, InsertExplicitColumnList) {
    auto s = parse_stmt("INSERT INTO t (a, b) VALUES (1, 2)");
    const auto& i = std::get<ast::InsertStmt>(s);
    ASSERT_EQ(i.columns.size(), 2u);
    EXPECT_EQ(i.columns[0], "a");
    EXPECT_EQ(i.columns[1], "b");
}

TEST(ParserDdlDmlTest, InsertMultipleRows) {
    auto s = parse_stmt("INSERT INTO t VALUES (1, 2), (3, 4), (5, 6)");
    const auto& i = std::get<ast::InsertStmt>(s);
    ASSERT_EQ(i.rows.size(), 3u);
    EXPECT_EQ(as<ast::IntLit>(*i.rows[0][0]).value, 1);
    EXPECT_EQ(as<ast::IntLit>(*i.rows[1][0]).value, 3);
    EXPECT_EQ(as<ast::IntLit>(*i.rows[2][0]).value, 5);
}

TEST(ParserDdlDmlTest, InsertNullValue) {
    auto s = parse_stmt("INSERT INTO t VALUES (1, NULL)");
    const auto& i = std::get<ast::InsertStmt>(s);
    ASSERT_EQ(i.rows.size(), 1u);
    EXPECT_TRUE(is<ast::NullLit>(*i.rows[0][1]));
}

TEST(ParserDdlDmlTest, InsertNegativeLiteral) {
    auto s = parse_stmt("INSERT INTO t VALUES (-42, -3.14)");
    const auto& i = std::get<ast::InsertStmt>(s);
    ASSERT_EQ(i.rows.size(), 1u);
    EXPECT_EQ(as<ast::IntLit>(*i.rows[0][0]).value, -42);
    EXPECT_DOUBLE_EQ(as<ast::DoubleLit>(*i.rows[0][1]).value, -3.14);
}

TEST(ParserDdlDmlTest, ErrorCreateWithoutTable) {
    auto r = try_parse_statement("CREATE (a INT)");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("TABLE"), std::string::npos);
}

TEST(ParserDdlDmlTest, ErrorCreateTableMissingParen) {
    auto r = try_parse_statement("CREATE TABLE t a INT");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("("), std::string::npos);
}

TEST(ParserDdlDmlTest, ErrorCreateTableUnknownType) {
    auto r = try_parse_statement("CREATE TABLE t (a TEXT)");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("type"), std::string::npos);
}

TEST(ParserDdlDmlTest, ErrorCreateTableNotWithoutNull) {
    auto r = try_parse_statement("CREATE TABLE t (a INT NOT)");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("NULL"), std::string::npos);
}

TEST(ParserDdlDmlTest, ErrorInsertMissingInto) {
    auto r = try_parse_statement("INSERT t VALUES (1)");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("INTO"), std::string::npos);
}

TEST(ParserDdlDmlTest, ErrorInsertMissingValues) {
    auto r = try_parse_statement("INSERT INTO t");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("VALUES"), std::string::npos);
}

TEST(ParserDdlDmlTest, ErrorInsertEmptyRow) {
    auto r = try_parse_statement("INSERT INTO t VALUES ()");
    ASSERT_TRUE(r.is_err());
}
