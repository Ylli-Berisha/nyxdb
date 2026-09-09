#include "parser/lexer.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

using namespace nyx;

static ast::ExprPtr parse(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    EXPECT_TRUE(toks.is_ok()) << toks.error().message;
    Parser p(src, std::move(toks.value()));
    auto r = p.parse_expression();
    EXPECT_TRUE(r.is_ok()) << r.error().message;
    return std::move(r.value());
}

static Result<ast::ExprPtr> try_parse(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    if (toks.is_err())
        return Result<ast::ExprPtr>::err(toks.error().message);
    Parser p(src, std::move(toks.value()));
    return p.parse_expression();
}

template <typename T> static const T& as(const ast::Expr& e) {
    return std::get<T>(e.node);
}

template <typename T> static bool is(const ast::Expr& e) {
    return std::holds_alternative<T>(e.node);
}

TEST(ParserExprTest, IntLiteral) {
    auto e = parse("42");
    ASSERT_TRUE(is<ast::IntLit>(*e));
    EXPECT_EQ(as<ast::IntLit>(*e).value, 42);
}

TEST(ParserExprTest, LargeIntLiteral) {
    auto e = parse("9223372036854775807");
    ASSERT_TRUE(is<ast::IntLit>(*e));
    EXPECT_EQ(as<ast::IntLit>(*e).value, 9223372036854775807LL);
}

TEST(ParserExprTest, DoubleLiteralFraction) {
    auto e = parse("3.14");
    ASSERT_TRUE(is<ast::DoubleLit>(*e));
    EXPECT_DOUBLE_EQ(as<ast::DoubleLit>(*e).value, 3.14);
}

TEST(ParserExprTest, DoubleLiteralExponent) {
    auto e = parse("1.5e3");
    ASSERT_TRUE(is<ast::DoubleLit>(*e));
    EXPECT_DOUBLE_EQ(as<ast::DoubleLit>(*e).value, 1500.0);
}

TEST(ParserExprTest, NullLiteral) {
    auto e = parse("NULL");
    EXPECT_TRUE(is<ast::NullLit>(*e));
}

TEST(ParserExprTest, BareColumnRef) {
    auto e = parse("foo");
    ASSERT_TRUE(is<ast::ColumnRef>(*e));
    const auto& c = as<ast::ColumnRef>(*e);
    EXPECT_FALSE(c.table.has_value());
    EXPECT_EQ(c.column, "foo");
}

TEST(ParserExprTest, DottedColumnRef) {
    auto e = parse("t.col");
    ASSERT_TRUE(is<ast::ColumnRef>(*e));
    const auto& c = as<ast::ColumnRef>(*e);
    ASSERT_TRUE(c.table.has_value());
    EXPECT_EQ(*c.table, "t");
    EXPECT_EQ(c.column, "col");
}

TEST(ParserExprTest, FuncCallNoArgs) {
    auto e = parse("now()");
    ASSERT_TRUE(is<ast::FuncCall>(*e));
    const auto& f = as<ast::FuncCall>(*e);
    EXPECT_EQ(f.name, "now");
    EXPECT_FALSE(f.star);
    EXPECT_EQ(f.args.size(), 0u);
}

TEST(ParserExprTest, FuncCallOneArg) {
    auto e = parse("sum(x)");
    ASSERT_TRUE(is<ast::FuncCall>(*e));
    const auto& f = as<ast::FuncCall>(*e);
    EXPECT_EQ(f.name, "sum");
    EXPECT_FALSE(f.star);
    ASSERT_EQ(f.args.size(), 1u);
    ASSERT_TRUE(is<ast::ColumnRef>(*f.args[0]));
    EXPECT_EQ(as<ast::ColumnRef>(*f.args[0]).column, "x");
}

TEST(ParserExprTest, FuncCallMultipleArgs) {
    auto e = parse("f(a, b, 3)");
    ASSERT_TRUE(is<ast::FuncCall>(*e));
    const auto& f = as<ast::FuncCall>(*e);
    EXPECT_EQ(f.name, "f");
    ASSERT_EQ(f.args.size(), 3u);
    EXPECT_TRUE(is<ast::ColumnRef>(*f.args[0]));
    EXPECT_TRUE(is<ast::ColumnRef>(*f.args[1]));
    EXPECT_TRUE(is<ast::IntLit>(*f.args[2]));
}

TEST(ParserExprTest, CountStar) {
    auto e = parse("COUNT(*)");
    ASSERT_TRUE(is<ast::FuncCall>(*e));
    const auto& f = as<ast::FuncCall>(*e);
    EXPECT_EQ(f.name, "count");
    EXPECT_TRUE(f.star);
    EXPECT_EQ(f.args.size(), 0u);
}

TEST(ParserExprTest, ParenthesizedLiteral) {
    auto e = parse("(42)");
    ASSERT_TRUE(is<ast::IntLit>(*e));
    EXPECT_EQ(as<ast::IntLit>(*e).value, 42);
}

TEST(ParserExprTest, SimpleAddition) {
    auto e = parse("1 + 2");
    ASSERT_TRUE(is<ast::BinaryOp>(*e));
    const auto& b = as<ast::BinaryOp>(*e);
    EXPECT_EQ(b.op, BinaryOpKind::ADD);
    EXPECT_TRUE(is<ast::IntLit>(*b.left));
    EXPECT_TRUE(is<ast::IntLit>(*b.right));
}

TEST(ParserExprTest, MulBindsTighterThanAdd) {
    auto e = parse("1 + 2 * 3");
    ASSERT_TRUE(is<ast::BinaryOp>(*e));
    const auto& outer = as<ast::BinaryOp>(*e);
    EXPECT_EQ(outer.op, BinaryOpKind::ADD);
    ASSERT_TRUE(is<ast::IntLit>(*outer.left));
    ASSERT_TRUE(is<ast::BinaryOp>(*outer.right));
    EXPECT_EQ(as<ast::BinaryOp>(*outer.right).op, BinaryOpKind::MUL);
}

TEST(ParserExprTest, ParensOverridePrecedence) {
    auto e = parse("(1 + 2) * 3");
    ASSERT_TRUE(is<ast::BinaryOp>(*e));
    const auto& outer = as<ast::BinaryOp>(*e);
    EXPECT_EQ(outer.op, BinaryOpKind::MUL);
    ASSERT_TRUE(is<ast::BinaryOp>(*outer.left));
    EXPECT_EQ(as<ast::BinaryOp>(*outer.left).op, BinaryOpKind::ADD);
}

TEST(ParserExprTest, SubIsLeftAssociative) {
    auto e = parse("a - b - c");
    ASSERT_TRUE(is<ast::BinaryOp>(*e));
    const auto& outer = as<ast::BinaryOp>(*e);
    EXPECT_EQ(outer.op, BinaryOpKind::SUB);
    ASSERT_TRUE(is<ast::BinaryOp>(*outer.left));
    EXPECT_EQ(as<ast::BinaryOp>(*outer.left).op, BinaryOpKind::SUB);
    EXPECT_TRUE(is<ast::ColumnRef>(*outer.right));
}

TEST(ParserExprTest, ComparisonProducesBinaryOp) {
    auto e = parse("a < b");
    ASSERT_TRUE(is<ast::BinaryOp>(*e));
    EXPECT_EQ(as<ast::BinaryOp>(*e).op, BinaryOpKind::LT);
}

TEST(ParserExprTest, AllComparisons) {
    for (auto& [src, kind] : std::vector<std::pair<std::string, BinaryOpKind>>{
             {"a < b", BinaryOpKind::LT},
             {"a <= b", BinaryOpKind::LE},
             {"a = b", BinaryOpKind::EQ},
             {"a <> b", BinaryOpKind::NE},
             {"a != b", BinaryOpKind::NE},
             {"a >= b", BinaryOpKind::GE},
             {"a > b", BinaryOpKind::GT},
         }) {
        auto e = parse(src);
        ASSERT_TRUE(is<ast::BinaryOp>(*e)) << src;
        EXPECT_EQ(as<ast::BinaryOp>(*e).op, kind) << src;
    }
}

TEST(ParserExprTest, LogicalAnd) {
    auto e = parse("a AND b");
    ASSERT_TRUE(is<ast::LogicalOp>(*e));
    EXPECT_EQ(as<ast::LogicalOp>(*e).op, LogicalOpKind::AND);
}

TEST(ParserExprTest, LogicalOr) {
    auto e = parse("a OR b");
    ASSERT_TRUE(is<ast::LogicalOp>(*e));
    EXPECT_EQ(as<ast::LogicalOp>(*e).op, LogicalOpKind::OR);
}

TEST(ParserExprTest, AndBindsTighterThanOr) {
    auto e = parse("a OR b AND c");
    ASSERT_TRUE(is<ast::LogicalOp>(*e));
    const auto& outer = as<ast::LogicalOp>(*e);
    EXPECT_EQ(outer.op, LogicalOpKind::OR);
    EXPECT_TRUE(is<ast::ColumnRef>(*outer.left));
    ASSERT_TRUE(is<ast::LogicalOp>(*outer.right));
    EXPECT_EQ(as<ast::LogicalOp>(*outer.right).op, LogicalOpKind::AND);
}

TEST(ParserExprTest, ComparisonBindsTighterThanAnd) {
    auto e = parse("a < b AND c > d");
    ASSERT_TRUE(is<ast::LogicalOp>(*e));
    const auto& outer = as<ast::LogicalOp>(*e);
    EXPECT_EQ(outer.op, LogicalOpKind::AND);
    ASSERT_TRUE(is<ast::BinaryOp>(*outer.left));
    ASSERT_TRUE(is<ast::BinaryOp>(*outer.right));
    EXPECT_EQ(as<ast::BinaryOp>(*outer.left).op, BinaryOpKind::LT);
    EXPECT_EQ(as<ast::BinaryOp>(*outer.right).op, BinaryOpKind::GT);
}

TEST(ParserExprTest, NotPrefix) {
    auto e = parse("NOT a");
    ASSERT_TRUE(is<ast::NotOp>(*e));
    EXPECT_TRUE(is<ast::ColumnRef>(*as<ast::NotOp>(*e).child));
}

TEST(ParserExprTest, NotBindsTighterThanAnd) {
    auto e = parse("NOT a AND b");
    ASSERT_TRUE(is<ast::LogicalOp>(*e));
    const auto& outer = as<ast::LogicalOp>(*e);
    EXPECT_EQ(outer.op, LogicalOpKind::AND);
    EXPECT_TRUE(is<ast::NotOp>(*outer.left));
    EXPECT_TRUE(is<ast::ColumnRef>(*outer.right));
}

TEST(ParserExprTest, IsNull) {
    auto e = parse("a IS NULL");
    ASSERT_TRUE(is<ast::NullCheck>(*e));
    const auto& n = as<ast::NullCheck>(*e);
    EXPECT_EQ(n.kind, NullCheckKind::IS_NULL);
    EXPECT_TRUE(is<ast::ColumnRef>(*n.child));
}

TEST(ParserExprTest, IsNotNull) {
    auto e = parse("a IS NOT NULL");
    ASSERT_TRUE(is<ast::NullCheck>(*e));
    EXPECT_EQ(as<ast::NullCheck>(*e).kind, NullCheckKind::IS_NOT_NULL);
}

TEST(ParserExprTest, IsNullBindsLooserThanArithmetic) {
    auto e = parse("a + b IS NULL");
    ASSERT_TRUE(is<ast::NullCheck>(*e));
    EXPECT_TRUE(is<ast::BinaryOp>(*as<ast::NullCheck>(*e).child));
}

TEST(ParserExprTest, IsNullBindsTighterThanAnd) {
    auto e = parse("a IS NULL AND b");
    ASSERT_TRUE(is<ast::LogicalOp>(*e));
    const auto& outer = as<ast::LogicalOp>(*e);
    EXPECT_TRUE(is<ast::NullCheck>(*outer.left));
    EXPECT_TRUE(is<ast::ColumnRef>(*outer.right));
}

TEST(ParserExprTest, UnaryMinusOnIntLiteralFolds) {
    auto e = parse("-42");
    ASSERT_TRUE(is<ast::IntLit>(*e));
    EXPECT_EQ(as<ast::IntLit>(*e).value, -42);
}

TEST(ParserExprTest, UnaryMinusOnDoubleLiteralFolds) {
    auto e = parse("-3.14");
    ASSERT_TRUE(is<ast::DoubleLit>(*e));
    EXPECT_DOUBLE_EQ(as<ast::DoubleLit>(*e).value, -3.14);
}

TEST(ParserExprTest, UnaryMinusOnExprLowersToZeroMinus) {
    auto e = parse("-(a + b)");
    ASSERT_TRUE(is<ast::BinaryOp>(*e));
    const auto& outer = as<ast::BinaryOp>(*e);
    EXPECT_EQ(outer.op, BinaryOpKind::SUB);
    ASSERT_TRUE(is<ast::IntLit>(*outer.left));
    EXPECT_EQ(as<ast::IntLit>(*outer.left).value, 0);
    EXPECT_TRUE(is<ast::BinaryOp>(*outer.right));
}

TEST(ParserExprTest, UnaryPlusIsNoop) {
    auto e = parse("+42");
    ASSERT_TRUE(is<ast::IntLit>(*e));
    EXPECT_EQ(as<ast::IntLit>(*e).value, 42);
}

TEST(ParserExprTest, ComplexExpression) {
    auto e = parse("a + b * c < d AND e OR NOT f IS NULL");
    ASSERT_TRUE(is<ast::LogicalOp>(*e));
    const auto& root = as<ast::LogicalOp>(*e);
    EXPECT_EQ(root.op, LogicalOpKind::OR);
    ASSERT_TRUE(is<ast::LogicalOp>(*root.left));
    EXPECT_EQ(as<ast::LogicalOp>(*root.left).op, LogicalOpKind::AND);
    ASSERT_TRUE(is<ast::NotOp>(*root.right));
    EXPECT_TRUE(is<ast::NullCheck>(*as<ast::NotOp>(*root.right).child));
}

TEST(ParserExprTest, SourceLocSpansWholeExpression) {
    std::string src = "1 + 2";
    auto e = parse(src);
    SourceLoc loc = ast::expr_loc(*e);
    EXPECT_EQ(loc.offset, 0u);
    EXPECT_EQ(loc.length, src.size());
}

TEST(ParserExprTest, ErrorTrailingTokens) {
    auto r = try_parse("1 + 2 rubbish");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unexpected token"), std::string::npos);
}

TEST(ParserExprTest, ErrorMissingCloseParen) {
    auto r = try_parse("(1 + 2");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find(")"), std::string::npos);
}

TEST(ParserExprTest, ErrorIsWithoutNull) {
    auto r = try_parse("a IS b");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("NULL"), std::string::npos);
}

TEST(ParserExprTest, ErrorDotWithoutIdentifier) {
    auto r = try_parse("t.");
    ASSERT_TRUE(r.is_err());
}

TEST(ParserExprTest, ErrorExpressionExpected) {
    auto r = try_parse("AND b");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("expression"), std::string::npos);
}

TEST(ParserExprTest, EmptyInputErrors) {
    auto r = try_parse("");
    ASSERT_TRUE(r.is_err());
}
