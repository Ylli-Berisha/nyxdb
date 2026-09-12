#include "binder/binder.h"
#include "catalog/catalog.h"
#include "parser/lexer.h"
#include "parser/parser.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_bind_select_full_test";

class BindSelectFullTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
        auto cat = Catalog::load(ROOT);
        ASSERT_TRUE(cat.is_ok());
        catalog_ = std::make_unique<Catalog>(std::move(cat.value()));

        Schema users{Column{"id", TypeId::INT32, false}, Column{"score", TypeId::DOUBLE, true}};
        ASSERT_TRUE(catalog_->add_table("users", users).is_ok());
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

// ── WHERE ──────────────────────────────────────────────────────────────────

TEST_F(BindSelectFullTest, WhereBindsColumnRef) {
    std::string src = "SELECT id FROM users WHERE id > 0";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_NE(r.value().where, nullptr);
    EXPECT_TRUE(is<bound::BoundBinaryOp>(*r.value().where));
}

TEST_F(BindSelectFullTest, WhereUnknownColumnErrors) {
    std::string src = "SELECT id FROM users WHERE ghost > 0";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown column"), std::string::npos);
}

TEST_F(BindSelectFullTest, WhereAggregateRejected) {
    std::string src = "SELECT id FROM users WHERE count(*) > 0";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("not allowed here"), std::string::npos);
}

// ── GROUP BY ───────────────────────────────────────────────────────────────

TEST_F(BindSelectFullTest, GroupBySingleColumn) {
    std::string src = "SELECT id FROM users GROUP BY id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().group_by.size(), 1u);
    EXPECT_TRUE(is<bound::BoundColumnRef>(*r.value().group_by[0]));
}

TEST_F(BindSelectFullTest, GroupByMultiColumn) {
    std::string src = "SELECT id FROM users GROUP BY id, score";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().group_by.size(), 2u);
}

TEST_F(BindSelectFullTest, GroupByUnknownColumnErrors) {
    std::string src = "SELECT id FROM users GROUP BY ghost";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("unknown column"), std::string::npos);
}

TEST_F(BindSelectFullTest, GroupByAggregateRejected) {
    std::string src = "SELECT count(*) FROM users GROUP BY count(id)";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("not allowed here"), std::string::npos);
}

// ── Aggregates in SELECT ────────────────────────────────────────────────────

TEST_F(BindSelectFullTest, AggregateCountStarTypeIsInt64) {
    std::string src = "SELECT count(*) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().aggregates.size(), 1u);
    EXPECT_EQ(r.value().aggregates[0].kind, AggregateKind::COUNT_STAR);
    EXPECT_EQ(r.value().aggregates[0].output_type, TypeId::INT64);
}

TEST_F(BindSelectFullTest, AggregateCountCol) {
    std::string src = "SELECT count(id) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().aggregates[0].kind, AggregateKind::COUNT);
    EXPECT_EQ(r.value().aggregates[0].output_type, TypeId::INT64);
    EXPECT_NE(r.value().aggregates[0].arg, nullptr);
}

TEST_F(BindSelectFullTest, AggregateSumIntOutputIsInt64) {
    std::string src = "SELECT sum(id) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().aggregates[0].kind, AggregateKind::SUM);
    EXPECT_EQ(r.value().aggregates[0].output_type, TypeId::INT64);
}

TEST_F(BindSelectFullTest, AggregateSumDoubleOutputIsDouble) {
    std::string src = "SELECT sum(score) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().aggregates[0].output_type, TypeId::DOUBLE);
}

TEST_F(BindSelectFullTest, AggregateAvgOutputIsDouble) {
    std::string src = "SELECT avg(id) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().aggregates[0].kind, AggregateKind::AVG);
    EXPECT_EQ(r.value().aggregates[0].output_type, TypeId::DOUBLE);
}

TEST_F(BindSelectFullTest, AggregateMinPreservesType) {
    std::string src = "SELECT min(id) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().aggregates[0].kind, AggregateKind::MIN);
    EXPECT_EQ(r.value().aggregates[0].output_type, TypeId::INT32);
}

TEST_F(BindSelectFullTest, AggregateMaxPreservesType) {
    std::string src = "SELECT max(score) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().aggregates[0].kind, AggregateKind::MAX);
    EXPECT_EQ(r.value().aggregates[0].output_type, TypeId::DOUBLE);
}

TEST_F(BindSelectFullTest, AggregateRefInProjection) {
    std::string src = "SELECT count(*) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().projections.size(), 1u);
    EXPECT_TRUE(is<bound::BoundAggregateRef>(*r.value().projections[0].expr));
    EXPECT_EQ(as<bound::BoundAggregateRef>(*r.value().projections[0].expr).aggregate_idx, 0u);
}

TEST_F(BindSelectFullTest, IsAggregatedFlagSetByAggregate) {
    std::string src = "SELECT count(*) FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(r.value().is_aggregated);
}

TEST_F(BindSelectFullTest, IsAggregatedFlagSetByGroupBy) {
    std::string src = "SELECT id FROM users GROUP BY id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_TRUE(r.value().is_aggregated);
}

TEST_F(BindSelectFullTest, NonAggregatedQueryFlagFalse) {
    std::string src = "SELECT id FROM users";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_FALSE(r.value().is_aggregated);
}

// ── Group-by validation ─────────────────────────────────────────────────────

TEST_F(BindSelectFullTest, GroupedColumnPassesValidation) {
    std::string src = "SELECT id, count(*) FROM users GROUP BY id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
}

TEST_F(BindSelectFullTest, UngroupedColumnErrors) {
    std::string src = "SELECT score, count(*) FROM users GROUP BY id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("non-aggregated column"), std::string::npos);
}

// ── HAVING ─────────────────────────────────────────────────────────────────

TEST_F(BindSelectFullTest, HavingWithAggregateBound) {
    std::string src = "SELECT count(*) FROM users HAVING count(*) > 0";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_NE(r.value().having, nullptr);
    EXPECT_TRUE(is<bound::BoundBinaryOp>(*r.value().having));
}

TEST_F(BindSelectFullTest, HavingAggregateRegistered) {
    std::string src = "SELECT count(*) FROM users HAVING count(*) > 0";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_GE(r.value().aggregates.size(), 1u);
}

// ── ORDER BY ───────────────────────────────────────────────────────────────

TEST_F(BindSelectFullTest, OrderByColumn) {
    std::string src = "SELECT id FROM users ORDER BY id";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().order_by.size(), 1u);
    EXPECT_TRUE(is<bound::BoundColumnRef>(*r.value().order_by[0].expr));
    EXPECT_TRUE(r.value().order_by[0].ascending);
}

TEST_F(BindSelectFullTest, OrderByDescending) {
    std::string src = "SELECT id FROM users ORDER BY id DESC";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_FALSE(r.value().order_by[0].ascending);
}

TEST_F(BindSelectFullTest, OrderByAliasEmitsProjectionRef) {
    std::string src = "SELECT id AS uid FROM users ORDER BY uid";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_EQ(r.value().order_by.size(), 1u);
    ASSERT_TRUE(is<bound::BoundProjectionRef>(*r.value().order_by[0].expr));
    EXPECT_EQ(as<bound::BoundProjectionRef>(*r.value().order_by[0].expr).proj_idx, 0u);
}

TEST_F(BindSelectFullTest, OrderByAggregateAlias) {
    std::string src = "SELECT count(*) AS cnt FROM users ORDER BY cnt";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_TRUE(is<bound::BoundProjectionRef>(*r.value().order_by[0].expr));
    EXPECT_EQ(as<bound::BoundProjectionRef>(*r.value().order_by[0].expr).proj_idx, 0u);
    EXPECT_EQ(as<bound::BoundProjectionRef>(*r.value().order_by[0].expr).type, TypeId::INT64);
}

// ── LIMIT / OFFSET ─────────────────────────────────────────────────────────

TEST_F(BindSelectFullTest, LimitSet) {
    std::string src = "SELECT id FROM users LIMIT 10";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_TRUE(r.value().limit.has_value());
    EXPECT_EQ(*r.value().limit, 10);
}

TEST_F(BindSelectFullTest, OffsetSet) {
    std::string src = "SELECT id FROM users LIMIT 10 OFFSET 5";
    auto stmt = parse_select(src);
    Binder b(*catalog_);
    auto r = b.bind_select(stmt, src);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    ASSERT_TRUE(r.value().offset.has_value());
    EXPECT_EQ(*r.value().offset, 5);
}
