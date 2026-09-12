#include "binder/binder.h"
#include "catalog/catalog.h"
#include "parser/lexer.h"
#include "parser/parser.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_bind_select_from_test";

class BindSelectFromTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
        auto cat = Catalog::load(ROOT);
        ASSERT_TRUE(cat.is_ok());
        catalog_ = std::make_unique<Catalog>(std::move(cat.value()));

        Schema users{Column{"id", TypeId::INT32, false}, Column{"score", TypeId::DOUBLE, true}};
        Schema orders{Column{"id", TypeId::INT64, false}, Column{"user_id", TypeId::INT32, false}};
        ASSERT_TRUE(catalog_->add_table("users", users).is_ok());
        ASSERT_TRUE(catalog_->add_table("orders", orders).is_ok());
    }
    void TearDown() override { fs::remove_all(ROOT); }

    std::unique_ptr<Catalog> catalog_;
};

static ast::SelectStmt parse_select(const std::string& src) {
    Lexer lex(src);
    auto toks = lex.tokenize();
    Parser p(src, std::move(toks.value()));
    auto stmts = p.parse();
    return std::move(std::get<ast::SelectStmt>(stmts.value()[0]));
}

template <typename T> static const T& as(const bound::BoundExpr& e) {
    return std::get<T>(e.node);
}

template <typename T> static bool is(const bound::BoundExpr& e) {
    return std::holds_alternative<T>(e.node);
}

TEST_F(BindSelectFromTest, SingleTableFromKnown) {
    auto stmt = parse_select("SELECT id FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT id FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().bindings.size(), 1u);
    EXPECT_EQ(r.value().bindings[0].table_name, "users");
}

TEST_F(BindSelectFromTest, UnknownTableErrors) {
    auto stmt = parse_select("SELECT id FROM ghost");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT id FROM ghost");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown table"), std::string::npos);
}

TEST_F(BindSelectFromTest, TableAliasStoredInBinding) {
    auto stmt = parse_select("SELECT u.id FROM users u");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT u.id FROM users u");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().bindings[0].alias, "u");
    EXPECT_EQ(r.value().bindings[0].table_name, "users");
}

TEST_F(BindSelectFromTest, NoAliasUsesTableName) {
    auto stmt = parse_select("SELECT id FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT id FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().bindings[0].alias, "users");
}

TEST_F(BindSelectFromTest, JoinAddsSecondBinding) {
    auto stmt = parse_select("SELECT users.id FROM users JOIN orders ON orders.user_id = users.id");
    Binder b(*catalog_);
    auto r =
        b.bind_select(stmt, "SELECT users.id FROM users JOIN orders ON orders.user_id = users.id");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().bindings.size(), 2u);
    EXPECT_EQ(r.value().bindings[1].table_name, "orders");
}

TEST_F(BindSelectFromTest, JoinProducesOnePredicate) {
    auto stmt = parse_select("SELECT users.id FROM users JOIN orders ON orders.user_id = users.id");
    Binder b(*catalog_);
    auto r =
        b.bind_select(stmt, "SELECT users.id FROM users JOIN orders ON orders.user_id = users.id");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().join_predicates.size(), 1u);
}

TEST_F(BindSelectFromTest, JoinOnPredicateBound) {
    std::string src = "SELECT users.id FROM users JOIN orders ON orders.user_id = users.id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(is<bound::BoundBinaryOp>(*r.value().join_predicates[0]));
}

TEST_F(BindSelectFromTest, JoinOnAmbiguousErrors) {
    std::string src = "SELECT users.id FROM users JOIN orders ON id = user_id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("ambiguous"), std::string::npos);
}

TEST_F(BindSelectFromTest, JoinUnknownTableErrors) {
    std::string src = "SELECT id FROM users JOIN ghost ON ghost.id = users.id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown table"), std::string::npos);
}

TEST_F(BindSelectFromTest, JoinOnUnknownColumnErrors) {
    std::string src = "SELECT users.id FROM users JOIN orders ON orders.nope = users.id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown column"), std::string::npos);
}

TEST_F(BindSelectFromTest, StarExpansionSingleTable) {
    auto stmt = parse_select("SELECT * FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT * FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().projections.size(), 2u);
}

TEST_F(BindSelectFromTest, StarExpansionColumnsAreColumnRefs) {
    auto stmt = parse_select("SELECT * FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT * FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    for (const auto& proj : r.value().projections)
        EXPECT_TRUE(is<bound::BoundColumnRef>(*proj.expr));
}

TEST_F(BindSelectFromTest, StarExpansionJoinCombinesBothSchemas) {
    std::string src = "SELECT * FROM users JOIN orders ON orders.user_id = users.id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().projections.size(), 4u);
}

TEST_F(BindSelectFromTest, StarExpansionBindingIdsCorrect) {
    std::string src = "SELECT * FROM users JOIN orders ON orders.user_id = users.id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto& projs = r.value().projections;
    EXPECT_EQ(as<bound::BoundColumnRef>(*projs[0].expr).ref.binding_id, 0u);
    EXPECT_EQ(as<bound::BoundColumnRef>(*projs[2].expr).ref.binding_id, 1u);
}

TEST_F(BindSelectFromTest, StarExpansionNoAlias) {
    auto stmt = parse_select("SELECT * FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT * FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    for (const auto& proj : r.value().projections)
        EXPECT_FALSE(proj.alias.has_value());
}

TEST_F(BindSelectFromTest, ExplicitProjectionSingleColumn) {
    auto stmt = parse_select("SELECT id FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT id FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().projections.size(), 1u);
    EXPECT_TRUE(is<bound::BoundColumnRef>(*r.value().projections[0].expr));
}

TEST_F(BindSelectFromTest, ProjectionWithAlias) {
    auto stmt = parse_select("SELECT id AS user_id FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT id AS user_id FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_TRUE(r.value().projections[0].alias.has_value());
    EXPECT_EQ(*r.value().projections[0].alias, "user_id");
}

TEST_F(BindSelectFromTest, MultipleProjections) {
    auto stmt = parse_select("SELECT id, score FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT id, score FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().projections.size(), 2u);
}

TEST_F(BindSelectFromTest, ProjectionUnknownColumnErrors) {
    auto stmt = parse_select("SELECT ghost FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT ghost FROM users");
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown column"), std::string::npos);
}

TEST_F(BindSelectFromTest, ProjectionQualifiedColumn) {
    auto stmt = parse_select("SELECT u.score FROM users u");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT u.score FROM users u");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto& ref = as<bound::BoundColumnRef>(*r.value().projections[0].expr).ref;
    EXPECT_EQ(ref.column_idx, 1u);
    EXPECT_EQ(ref.type, TypeId::DOUBLE);
}

TEST_F(BindSelectFromTest, ProjectionTypePropagated) {
    auto stmt = parse_select("SELECT id FROM users");
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, "SELECT id FROM users");
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(bound::bound_expr_type(*r.value().projections[0].expr), TypeId::INT32);
}

TEST_F(BindSelectFromTest, ProjectionExpression) {
    std::string src = "SELECT id + 1 FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(is<bound::BoundBinaryOp>(*r.value().projections[0].expr));
    EXPECT_EQ(bound::bound_expr_type(*r.value().projections[0].expr), TypeId::INT32);
}
