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

TEST(ParserSelectFullTest, ImplicitInnerJoin) {
    auto s = parse_select("SELECT * FROM a JOIN b ON a.id = b.id");
    ASSERT_EQ(s.joins.size(), 1u);
    EXPECT_EQ(s.joins[0].right.table_name, "b");
    ASSERT_NE(s.joins[0].on, nullptr);
    EXPECT_TRUE(is<ast::BinaryOp>(*s.joins[0].on));
    EXPECT_EQ(as<ast::BinaryOp>(*s.joins[0].on).op, BinaryOpKind::EQ);
}

TEST(ParserSelectFullTest, ExplicitInnerJoin) {
    auto s = parse_select("SELECT * FROM a INNER JOIN b ON a.id = b.id");
    ASSERT_EQ(s.joins.size(), 1u);
    EXPECT_EQ(s.joins[0].right.table_name, "b");
}

TEST(ParserSelectFullTest, JoinWithAlias) {
    auto s = parse_select("SELECT * FROM a JOIN b AS x ON a.id = x.id");
    ASSERT_EQ(s.joins.size(), 1u);
    EXPECT_EQ(s.joins[0].right.table_name, "b");
    ASSERT_TRUE(s.joins[0].right.alias.has_value());
    EXPECT_EQ(*s.joins[0].right.alias, "x");
}

TEST(ParserSelectFullTest, MultipleJoins) {
    auto s = parse_select("SELECT * FROM a JOIN b ON a.id = b.id JOIN c ON b.id = c.id");
    ASSERT_EQ(s.joins.size(), 2u);
    EXPECT_EQ(s.joins[0].right.table_name, "b");
    EXPECT_EQ(s.joins[1].right.table_name, "c");
}

TEST(ParserSelectFullTest, GroupBySingle) {
    auto s = parse_select("SELECT cat FROM t GROUP BY cat");
    ASSERT_EQ(s.group_by.size(), 1u);
    ASSERT_TRUE(is<ast::ColumnRef>(*s.group_by[0]));
    EXPECT_EQ(as<ast::ColumnRef>(*s.group_by[0]).column, "cat");
}

TEST(ParserSelectFullTest, GroupByMultiple) {
    auto s = parse_select("SELECT * FROM t GROUP BY a, b, c");
    ASSERT_EQ(s.group_by.size(), 3u);
    EXPECT_EQ(as<ast::ColumnRef>(*s.group_by[0]).column, "a");
    EXPECT_EQ(as<ast::ColumnRef>(*s.group_by[1]).column, "b");
    EXPECT_EQ(as<ast::ColumnRef>(*s.group_by[2]).column, "c");
}

TEST(ParserSelectFullTest, GroupByExpression) {
    auto s = parse_select("SELECT * FROM t GROUP BY a + b");
    ASSERT_EQ(s.group_by.size(), 1u);
    ASSERT_TRUE(is<ast::BinaryOp>(*s.group_by[0]));
}

TEST(ParserSelectFullTest, Having) {
    auto s = parse_select("SELECT * FROM t GROUP BY cat HAVING count(*) > 5");
    ASSERT_NE(s.having, nullptr);
    ASSERT_TRUE(is<ast::BinaryOp>(*s.having));
    EXPECT_EQ(as<ast::BinaryOp>(*s.having).op, BinaryOpKind::GT);
}

TEST(ParserSelectFullTest, OrderBySingleImplicitAsc) {
    auto s = parse_select("SELECT * FROM t ORDER BY id");
    ASSERT_EQ(s.order_by.size(), 1u);
    EXPECT_TRUE(s.order_by[0].ascending);
    EXPECT_EQ(as<ast::ColumnRef>(*s.order_by[0].expr).column, "id");
}

TEST(ParserSelectFullTest, OrderByExplicitAsc) {
    auto s = parse_select("SELECT * FROM t ORDER BY id ASC");
    ASSERT_EQ(s.order_by.size(), 1u);
    EXPECT_TRUE(s.order_by[0].ascending);
}

TEST(ParserSelectFullTest, OrderByDesc) {
    auto s = parse_select("SELECT * FROM t ORDER BY id DESC");
    ASSERT_EQ(s.order_by.size(), 1u);
    EXPECT_FALSE(s.order_by[0].ascending);
}

TEST(ParserSelectFullTest, OrderByMultiple) {
    auto s = parse_select("SELECT * FROM t ORDER BY a ASC, b DESC, c");
    ASSERT_EQ(s.order_by.size(), 3u);
    EXPECT_TRUE(s.order_by[0].ascending);
    EXPECT_FALSE(s.order_by[1].ascending);
    EXPECT_TRUE(s.order_by[2].ascending);
}

TEST(ParserSelectFullTest, Limit) {
    auto s = parse_select("SELECT * FROM t LIMIT 10");
    ASSERT_TRUE(s.limit.has_value());
    EXPECT_EQ(*s.limit, 10);
    EXPECT_FALSE(s.offset.has_value());
}

TEST(ParserSelectFullTest, LimitOffset) {
    auto s = parse_select("SELECT * FROM t LIMIT 10 OFFSET 5");
    ASSERT_TRUE(s.limit.has_value());
    ASSERT_TRUE(s.offset.has_value());
    EXPECT_EQ(*s.limit, 10);
    EXPECT_EQ(*s.offset, 5);
}

TEST(ParserSelectFullTest, OffsetOnly) {
    auto s = parse_select("SELECT * FROM t OFFSET 20");
    EXPECT_FALSE(s.limit.has_value());
    ASSERT_TRUE(s.offset.has_value());
    EXPECT_EQ(*s.offset, 20);
}

TEST(ParserSelectFullTest, FullPipelineShape) {
    auto s = parse_select("SELECT cat, count(*) AS n FROM events e WHERE ts > 1000 "
                          "GROUP BY cat HAVING count(*) > 10 ORDER BY n DESC LIMIT 100 OFFSET 5");
    EXPECT_EQ(s.projections.size(), 2u);
    EXPECT_EQ(s.from.table_name, "events");
    ASSERT_TRUE(s.from.alias.has_value());
    EXPECT_EQ(*s.from.alias, "e");
    EXPECT_NE(s.where, nullptr);
    EXPECT_EQ(s.group_by.size(), 1u);
    EXPECT_NE(s.having, nullptr);
    EXPECT_EQ(s.order_by.size(), 1u);
    EXPECT_FALSE(s.order_by[0].ascending);
    ASSERT_TRUE(s.limit.has_value());
    EXPECT_EQ(*s.limit, 100);
    ASSERT_TRUE(s.offset.has_value());
    EXPECT_EQ(*s.offset, 5);
}

TEST(ParserSelectFullTest, WhereBeforeGroupBy) {
    auto s = parse_select("SELECT cat FROM t WHERE id > 0 GROUP BY cat");
    EXPECT_NE(s.where, nullptr);
    EXPECT_EQ(s.group_by.size(), 1u);
}

TEST(ParserSelectFullTest, ClauseOrderMatters) {
    auto r = try_parse_statement("SELECT * FROM t GROUP BY a WHERE b > 0");
    EXPECT_TRUE(r.is_err());
}

TEST(ParserSelectFullTest, ErrorJoinWithoutOn) {
    auto r = try_parse_statement("SELECT * FROM a JOIN b");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("ON"), std::string::npos);
}

TEST(ParserSelectFullTest, ErrorGroupWithoutBy) {
    auto r = try_parse_statement("SELECT * FROM t GROUP cat");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("BY"), std::string::npos);
}

TEST(ParserSelectFullTest, ErrorOrderWithoutBy) {
    auto r = try_parse_statement("SELECT * FROM t ORDER cat");
    ASSERT_TRUE(r.is_err());
}

TEST(ParserSelectFullTest, ErrorLimitWithoutNumber) {
    auto r = try_parse_statement("SELECT * FROM t LIMIT");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("LIMIT"), std::string::npos);
}

TEST(ParserSelectFullTest, ErrorOffsetWithoutNumber) {
    auto r = try_parse_statement("SELECT * FROM t OFFSET");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("OFFSET"), std::string::npos);
}

TEST(ParserSelectFullTest, ErrorHavingWithoutExpression) {
    auto r = try_parse_statement("SELECT * FROM t GROUP BY cat HAVING");
    ASSERT_TRUE(r.is_err());
}
