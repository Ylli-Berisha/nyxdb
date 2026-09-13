#include "binder/binder.h"
#include "catalog/catalog.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "storage/disk/value.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_planner_test";

class PlannerTest : public ::testing::Test {
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

    bound::BoundSelect bind(const std::string& src) {
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(src, std::move(toks.value()));
        auto stmts = p.parse();
        Binder b(*catalog_);
        auto r = b.bind_select(std::get<ast::SelectStmt>(stmts.value()[0]), src);
        return std::move(r.value());
    }

    void insert(std::vector<std::vector<Value>> rows) {
        ASSERT_TRUE(catalog_->table("users")->insert_many(rows).is_ok());
    }

    static size_t drain(Operator& op) {
        EXPECT_TRUE(op.open().is_ok());
        size_t n = 0;
        while (true) {
            auto r = op.next();
            EXPECT_TRUE(r.is_ok());
            if (!r.value())
                break;
            n += r.value()->row_count();
        }
        return n;
    }

    static std::vector<i32> drain_i32(Operator& op, size_t col = 0) {
        EXPECT_TRUE(op.open().is_ok());
        std::vector<i32> vals;
        while (true) {
            auto r = op.next();
            EXPECT_TRUE(r.is_ok());
            if (!r.value())
                break;
            auto& chunk = *r.value();
            for (size_t i = 0; i < chunk.row_count(); ++i)
                vals.push_back(chunk.column(col).get_i32(i));
        }
        return vals;
    }

    std::unique_ptr<Catalog> catalog_;
};

TEST_F(PlannerTest, PlanReturnsOk) {
    auto stmt = bind("SELECT id FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
}

TEST_F(PlannerTest, UnknownTableErrors) {
    Lexer lex("SELECT id FROM ghost");
    auto toks = lex.tokenize();
    Parser p("SELECT id FROM ghost", std::move(toks.value()));
    auto stmts = p.parse();
    Binder b(*catalog_);
    auto bound_r =
        b.bind_select(std::get<ast::SelectStmt>(stmts.value()[0]), "SELECT id FROM ghost");
    ASSERT_TRUE(bound_r.is_err());
}

TEST_F(PlannerTest, OutputSchemaColumnCount) {
    auto stmt = bind("SELECT id, score FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->output_schema().size(), 2u);
}

TEST_F(PlannerTest, OutputSchemaColumnName) {
    auto stmt = bind("SELECT id FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->output_schema()[0].name, "id");
}

TEST_F(PlannerTest, OutputSchemaColumnType) {
    auto stmt = bind("SELECT score FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->output_schema()[0].type, TypeId::DOUBLE);
}

TEST_F(PlannerTest, AliasInOutputSchema) {
    auto stmt = bind("SELECT id AS uid FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->output_schema()[0].name, "uid");
}

TEST_F(PlannerTest, EmptyTableReturnsNoRows) {
    auto stmt = bind("SELECT id FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 0u);
}

TEST_F(PlannerTest, ScansAllRows) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT id FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 3u);
}

TEST_F(PlannerTest, ProjectionExpressionType) {
    auto stmt = bind("SELECT id + 1 FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->output_schema()[0].type, TypeId::INT32);
}

TEST_F(PlannerTest, WhereFiltersRows) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT id FROM users WHERE id > 1");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_i32(*op);
    ASSERT_EQ(vals.size(), 2u);
    for (i32 v : vals)
        EXPECT_GT(v, 1);
}

TEST_F(PlannerTest, OrderByAscending) {
    insert({{Value{i32(3)}, Value{f64(3.0)}},
            {Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}}});
    auto stmt = bind("SELECT id FROM users ORDER BY id ASC");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_i32(*op);
    ASSERT_EQ(vals.size(), 3u);
    EXPECT_EQ(vals[0], 1);
    EXPECT_EQ(vals[1], 2);
    EXPECT_EQ(vals[2], 3);
}

TEST_F(PlannerTest, OrderByDescending) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(3)}, Value{f64(3.0)}},
            {Value{i32(2)}, Value{f64(2.0)}}});
    auto stmt = bind("SELECT id FROM users ORDER BY id DESC");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_i32(*op);
    ASSERT_EQ(vals.size(), 3u);
    EXPECT_EQ(vals[0], 3);
    EXPECT_EQ(vals[1], 2);
    EXPECT_EQ(vals[2], 1);
}

TEST_F(PlannerTest, LimitReducesRows) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}},
            {Value{i32(4)}, Value{f64(4.0)}},
            {Value{i32(5)}, Value{f64(5.0)}}});
    auto stmt = bind("SELECT id FROM users LIMIT 2");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 2u);
}

TEST_F(PlannerTest, LimitWithOffset) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}},
            {Value{i32(4)}, Value{f64(4.0)}},
            {Value{i32(5)}, Value{f64(5.0)}}});
    auto stmt = bind("SELECT id FROM users ORDER BY id ASC LIMIT 2 OFFSET 2");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_i32(*op);
    ASSERT_EQ(vals.size(), 2u);
    EXPECT_EQ(vals[0], 3);
    EXPECT_EQ(vals[1], 4);
}
