#include "parser/lexer.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

using namespace nyx;

static ast::SelectStmt parse_select(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    EXPECT_TRUE(toks.is_ok()) << toks.error().message;
    Parser p(src, std::move(toks.value()));
    auto r = p.parse_statement();
    EXPECT_TRUE(r.is_ok()) << r.error().message;
    return std::get<ast::SelectStmt>(std::move(r.value()));
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

TEST(ParserSelectTest, SelectSingleColumn) {
    auto s = parse_select("SELECT id FROM users");
    EXPECT_FALSE(s.star_projection);
    ASSERT_EQ(s.projections.size(), 1u);
    ASSERT_TRUE(is<ast::ColumnRef>(*s.projections[0].expr));
    EXPECT_EQ(as<ast::ColumnRef>(*s.projections[0].expr).column, "id");
    EXPECT_FALSE(s.projections[0].alias.has_value());
    EXPECT_EQ(s.from.table_name, "users");
    EXPECT_FALSE(s.from.alias.has_value());
    EXPECT_EQ(s.where, nullptr);
}

TEST(ParserSelectTest, SelectMultipleColumns) {
    auto s = parse_select("SELECT id, name, age FROM users");
    ASSERT_EQ(s.projections.size(), 3u);
    EXPECT_EQ(as<ast::ColumnRef>(*s.projections[0].expr).column, "id");
    EXPECT_EQ(as<ast::ColumnRef>(*s.projections[1].expr).column, "name");
    EXPECT_EQ(as<ast::ColumnRef>(*s.projections[2].expr).column, "age");
}

TEST(ParserSelectTest, SelectStar) {
    auto s = parse_select("SELECT * FROM users");
    EXPECT_TRUE(s.star_projection);
    EXPECT_EQ(s.projections.size(), 0u);
    EXPECT_EQ(s.from.table_name, "users");
}

TEST(ParserSelectTest, SelectExpressionProjection) {
    auto s = parse_select("SELECT a + b, c * 2 FROM t");
    ASSERT_EQ(s.projections.size(), 2u);
    ASSERT_TRUE(is<ast::BinaryOp>(*s.projections[0].expr));
    EXPECT_EQ(as<ast::BinaryOp>(*s.projections[0].expr).op, BinaryOpKind::ADD);
    ASSERT_TRUE(is<ast::BinaryOp>(*s.projections[1].expr));
    EXPECT_EQ(as<ast::BinaryOp>(*s.projections[1].expr).op, BinaryOpKind::MUL);
}

TEST(ParserSelectTest, SelectExplicitColumnAlias) {
    auto s = parse_select("SELECT a AS x FROM t");
    ASSERT_EQ(s.projections.size(), 1u);
    ASSERT_TRUE(s.projections[0].alias.has_value());
    EXPECT_EQ(*s.projections[0].alias, "x");
}

TEST(ParserSelectTest, SelectImplicitColumnAlias) {
    auto s = parse_select("SELECT a x FROM t");
    ASSERT_EQ(s.projections.size(), 1u);
    ASSERT_TRUE(s.projections[0].alias.has_value());
    EXPECT_EQ(*s.projections[0].alias, "x");
}

TEST(ParserSelectTest, SelectMixedAliases) {
    auto s = parse_select("SELECT a AS x, b y, c FROM t");
    ASSERT_EQ(s.projections.size(), 3u);
    EXPECT_EQ(*s.projections[0].alias, "x");
    EXPECT_EQ(*s.projections[1].alias, "y");
    EXPECT_FALSE(s.projections[2].alias.has_value());
}

TEST(ParserSelectTest, SelectExplicitTableAlias) {
    auto s = parse_select("SELECT * FROM users AS u");
    EXPECT_EQ(s.from.table_name, "users");
    ASSERT_TRUE(s.from.alias.has_value());
    EXPECT_EQ(*s.from.alias, "u");
}

TEST(ParserSelectTest, SelectImplicitTableAlias) {
    auto s = parse_select("SELECT * FROM users u");
    EXPECT_EQ(s.from.table_name, "users");
    ASSERT_TRUE(s.from.alias.has_value());
    EXPECT_EQ(*s.from.alias, "u");
}

TEST(ParserSelectTest, SelectWithWhere) {
    auto s = parse_select("SELECT id FROM users WHERE id = 5");
    ASSERT_NE(s.where, nullptr);
    ASSERT_TRUE(is<ast::BinaryOp>(*s.where));
    EXPECT_EQ(as<ast::BinaryOp>(*s.where).op, BinaryOpKind::EQ);
}

TEST(ParserSelectTest, SelectWithComplexWhere) {
    auto s = parse_select("SELECT * FROM t WHERE a > 5 AND b < 10 OR c IS NULL");
    ASSERT_NE(s.where, nullptr);
    ASSERT_TRUE(is<ast::LogicalOp>(*s.where));
    EXPECT_EQ(as<ast::LogicalOp>(*s.where).op, LogicalOpKind::OR);
}

TEST(ParserSelectTest, SelectDottedColumn) {
    auto s = parse_select("SELECT t.id FROM t");
    ASSERT_EQ(s.projections.size(), 1u);
    ASSERT_TRUE(is<ast::ColumnRef>(*s.projections[0].expr));
    const auto& col = as<ast::ColumnRef>(*s.projections[0].expr);
    ASSERT_TRUE(col.table.has_value());
    EXPECT_EQ(*col.table, "t");
    EXPECT_EQ(col.column, "id");
}

TEST(ParserSelectTest, SelectFuncCallProjection) {
    auto s = parse_select("SELECT count(*), sum(x) FROM t");
    ASSERT_EQ(s.projections.size(), 2u);
    ASSERT_TRUE(is<ast::FuncCall>(*s.projections[0].expr));
    EXPECT_EQ(as<ast::FuncCall>(*s.projections[0].expr).name, "count");
    EXPECT_TRUE(as<ast::FuncCall>(*s.projections[0].expr).star);
    ASSERT_TRUE(is<ast::FuncCall>(*s.projections[1].expr));
    EXPECT_EQ(as<ast::FuncCall>(*s.projections[1].expr).name, "sum");
}

TEST(ParserSelectTest, TrailingSemicolonAllowed) {
    auto s = parse_select("SELECT id FROM t;");
    EXPECT_EQ(s.from.table_name, "t");
}

TEST(ParserSelectTest, KeywordsAreCaseInsensitive) {
    auto s = parse_select("select ID from Users where ID = 5");
    ASSERT_EQ(s.projections.size(), 1u);
    EXPECT_EQ(as<ast::ColumnRef>(*s.projections[0].expr).column, "id");
    EXPECT_EQ(s.from.table_name, "users");
    ASSERT_NE(s.where, nullptr);
}

TEST(ParserSelectTest, ErrorMissingFrom) {
    auto r = try_parse_statement("SELECT id");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("FROM"), std::string::npos);
}

TEST(ParserSelectTest, ErrorMissingTable) {
    auto r = try_parse_statement("SELECT id FROM");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("table"), std::string::npos);
}

TEST(ParserSelectTest, ErrorEmptyProjectionList) {
    auto r = try_parse_statement("SELECT FROM t");
    ASSERT_TRUE(r.is_err());
}

TEST(ParserSelectTest, ErrorTrailingJunk) {
    auto r = try_parse_statement("SELECT id FROM t x y");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unexpected"), std::string::npos);
}

TEST(ParserSelectTest, ErrorMissingWhereExpression) {
    auto r = try_parse_statement("SELECT id FROM t WHERE");
    ASSERT_TRUE(r.is_err());
}
