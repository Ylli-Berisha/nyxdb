#include "binder/binder.h"
#include "catalog/catalog.h"
#include "parser/lexer.h"
#include "parser/parser.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string BIND_ROOT = "/tmp/nyxdb_binder_expr_test";

class BindExprTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(BIND_ROOT);
        fs::create_directories(BIND_ROOT);
    }
    void TearDown() override { fs::remove_all(BIND_ROOT); }
};

static ast::ExprPtr parse_expr(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    EXPECT_TRUE(toks.is_ok()) << toks.error().message;
    Parser p(src, std::move(toks.value()));
    auto r = p.parse_expression();
    EXPECT_TRUE(r.is_ok()) << r.error().message;
    return std::move(r.value());
}

template <typename T> static const T& as(const bound::BoundExpr& e) {
    return std::get<T>(e.node);
}

template <typename T> static bool is(const bound::BoundExpr& e) {
    return std::holds_alternative<T>(e.node);
}

TEST_F(BindExprTest, SmallIntLitIsInt32) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    auto e = parse_expr("42");
    auto r = b.bind_expression(*e, {}, "42");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_TRUE(is<bound::BoundIntLit>(*r.value()));
    EXPECT_EQ(as<bound::BoundIntLit>(*r.value()).type, TypeId::INT32);
    EXPECT_EQ(as<bound::BoundIntLit>(*r.value()).value, 42);
}

TEST_F(BindExprTest, LargeIntLitIsInt64) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    std::string src = "9999999999";
    auto e = parse_expr(src);
    auto r = b.bind_expression(*e, {}, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(as<bound::BoundIntLit>(*r.value()).type, TypeId::INT64);
}

TEST_F(BindExprTest, DoubleLitIsDouble) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    auto e = parse_expr("3.14");
    auto r = b.bind_expression(*e, {}, "3.14");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(is<bound::BoundDoubleLit>(*r.value()));
    EXPECT_EQ(bound::bound_expr_type(*r.value()), TypeId::DOUBLE);
}

TEST_F(BindExprTest, NullLitDefaultsToInt32) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    auto e = parse_expr("NULL");
    auto r = b.bind_expression(*e, {}, "NULL");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(is<bound::BoundNullLit>(*r.value()));
    EXPECT_TRUE(bound::bound_expr_nullable(*r.value()));
}

static Schema users_schema() {
    return Schema{
        Column{"id", TypeId::INT32, false},
        Column{"score", TypeId::DOUBLE, true},
    };
}

static Schema orders_schema() {
    return Schema{
        Column{"id", TypeId::INT64, false},
        Column{"user_id", TypeId::INT32, false},
    };
}

TEST_F(BindExprTest, BareColumnResolvesWhenUnique) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema s = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "users", &s}};
    auto e = parse_expr("id");
    auto r = b.bind_expression(*e, bindings, "id");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_TRUE(is<bound::BoundColumnRef>(*r.value()));
    const auto& c = as<bound::BoundColumnRef>(*r.value()).ref;
    EXPECT_EQ(c.binding_id, 0u);
    EXPECT_EQ(c.column_idx, 0u);
    EXPECT_EQ(c.type, TypeId::INT32);
    EXPECT_FALSE(c.nullable);
}

TEST_F(BindExprTest, BareColumnAmbiguousAcrossBindingsErrors) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    Schema o = orders_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "users", &u}, {"orders", "orders", &o}};
    auto e = parse_expr("id");
    auto r = b.bind_expression(*e, bindings, "id");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("ambiguous"), std::string::npos);
}

TEST_F(BindExprTest, QualifiedColumnByTableName) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    Schema o = orders_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "users", &u}, {"orders", "orders", &o}};
    auto e = parse_expr("orders.id");
    auto r = b.bind_expression(*e, bindings, "orders.id");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto& c = as<bound::BoundColumnRef>(*r.value()).ref;
    EXPECT_EQ(c.binding_id, 1u);
    EXPECT_EQ(c.type, TypeId::INT64);
}

TEST_F(BindExprTest, QualifiedColumnByAlias) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.score");
    auto r = b.bind_expression(*e, bindings, "u.score");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto& c = as<bound::BoundColumnRef>(*r.value()).ref;
    EXPECT_EQ(c.column_idx, 1u);
    EXPECT_EQ(c.type, TypeId::DOUBLE);
    EXPECT_TRUE(c.nullable);
}

TEST_F(BindExprTest, UnknownColumnErrors) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "users", &u}};
    auto e = parse_expr("ghost");
    auto r = b.bind_expression(*e, bindings, "ghost");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown column"), std::string::npos);
}

TEST_F(BindExprTest, UnknownTableErrors) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "users", &u}};
    auto e = parse_expr("nope.id");
    auto r = b.bind_expression(*e, bindings, "nope.id");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown column"), std::string::npos);
}

TEST_F(BindExprTest, BinaryOpSameTypeOK) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.id + 1");
    auto r = b.bind_expression(*e, bindings, "u.id + 1");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_TRUE(is<bound::BoundBinaryOp>(*r.value()));
    const auto& bop = as<bound::BoundBinaryOp>(*r.value());
    EXPECT_EQ(bop.operand_type, TypeId::INT32);
    EXPECT_EQ(bop.result_type, TypeId::INT32);
}

TEST_F(BindExprTest, IntLitNarrowsToInt32ForColumn) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.id = 42");
    auto r = b.bind_expression(*e, bindings, "u.id = 42");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto& bop = as<bound::BoundBinaryOp>(*r.value());
    EXPECT_EQ(bop.operand_type, TypeId::INT32);
    EXPECT_EQ(bop.result_type, TypeId::INT32);
}

TEST_F(BindExprTest, IntLitWidensToInt64ForColumn) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema o = orders_schema();
    std::vector<bound::BoundBinding> bindings = {{"orders", "o", &o}};
    auto e = parse_expr("o.id > 5");
    auto r = b.bind_expression(*e, bindings, "o.id > 5");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto& bop = as<bound::BoundBinaryOp>(*r.value());
    EXPECT_EQ(bop.operand_type, TypeId::INT64);
}

TEST_F(BindExprTest, IntVsDoubleErrors) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.score + 1");
    auto r = b.bind_expression(*e, bindings, "u.score + 1");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("type mismatch"), std::string::npos);
}

TEST_F(BindExprTest, ComparisonResultIsInt32) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.id < 100");
    auto r = b.bind_expression(*e, bindings, "u.id < 100");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(bound::bound_expr_type(*r.value()), TypeId::INT32);
}

TEST_F(BindExprTest, ArithmeticResultMatchesOperand) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.score * 2.0");
    auto r = b.bind_expression(*e, bindings, "u.score * 2.0");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(bound::bound_expr_type(*r.value()), TypeId::DOUBLE);
}

TEST_F(BindExprTest, LogicalAndOnBooleans) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.id > 0 AND u.id < 100");
    auto r = b.bind_expression(*e, bindings, "u.id > 0 AND u.id < 100");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(is<bound::BoundLogicalOp>(*r.value()));
}

TEST_F(BindExprTest, LogicalOnNonBooleanErrors) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.score AND u.id > 0");
    auto r = b.bind_expression(*e, bindings, "u.score AND u.id > 0");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("boolean"), std::string::npos);
}

TEST_F(BindExprTest, NotOnBoolean) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("NOT u.id > 5");
    auto r = b.bind_expression(*e, bindings, "NOT u.id > 5");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(is<bound::BoundNotOp>(*r.value()));
}

TEST_F(BindExprTest, NotOnNonBooleanErrors) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("NOT u.score");
    auto r = b.bind_expression(*e, bindings, "NOT u.score");
    ASSERT_TRUE(r.is_err());
}

TEST_F(BindExprTest, IsNullOnAnyType) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.score IS NULL");
    auto r = b.bind_expression(*e, bindings, "u.score IS NULL");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(is<bound::BoundNullCheck>(*r.value()));
    EXPECT_EQ(bound::bound_expr_type(*r.value()), TypeId::INT32);
    EXPECT_FALSE(bound::bound_expr_nullable(*r.value()));
}

TEST_F(BindExprTest, AggregateFuncCallRejectedHere) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("count(u.id)");
    auto r = b.bind_expression(*e, bindings, "count(u.id)");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("aggregate"), std::string::npos);
    EXPECT_NE(r.error().message.find("not allowed here"), std::string::npos);
}

TEST_F(BindExprTest, UnknownFuncCall) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("substring(u.id)");
    auto r = b.bind_expression(*e, bindings, "substring(u.id)");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown function"), std::string::npos);
}

TEST_F(BindExprTest, NestedExpression) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    std::string src = "(u.id + 1) * 2";
    auto e = parse_expr(src);
    auto r = b.bind_expression(*e, bindings, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto& outer = as<bound::BoundBinaryOp>(*r.value());
    EXPECT_EQ(outer.op, BinaryOpKind::MUL);
    EXPECT_EQ(outer.operand_type, TypeId::INT32);
}

TEST_F(BindExprTest, NullableThroughBinaryOp) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("u.score + 1.0");
    auto r = b.bind_expression(*e, bindings, "u.score + 1.0");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(bound::bound_expr_nullable(*r.value()));
}

TEST_F(BindExprTest, ErrorLocationHasLineCol) {
    auto cat = Catalog::load(BIND_ROOT);
    Binder b(cat.value());
    Schema u = users_schema();
    std::vector<bound::BoundBinding> bindings = {{"users", "u", &u}};
    auto e = parse_expr("ghost");
    auto r = b.bind_expression(*e, bindings, "ghost");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("bind error"), std::string::npos);
    EXPECT_NE(r.error().message.find("line 1"), std::string::npos);
    EXPECT_NE(r.error().message.find("^"), std::string::npos);
}
