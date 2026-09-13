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

static const std::string ROOT = "/tmp/nyxdb_planner_agg_test";

class PlannerAggTest : public ::testing::Test {
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

    static std::vector<i64> drain_i64(Operator& op, size_t col = 0) {
        EXPECT_TRUE(op.open().is_ok());
        std::vector<i64> vals;
        while (true) {
            auto r = op.next();
            EXPECT_TRUE(r.is_ok());
            if (!r.value())
                break;
            auto& chunk = *r.value();
            for (size_t i = 0; i < chunk.row_count(); ++i)
                vals.push_back(chunk.column(col).get_i64(i));
        }
        return vals;
    }

    static std::vector<f64> drain_f64(Operator& op, size_t col = 0) {
        EXPECT_TRUE(op.open().is_ok());
        std::vector<f64> vals;
        while (true) {
            auto r = op.next();
            EXPECT_TRUE(r.is_ok());
            if (!r.value())
                break;
            auto& chunk = *r.value();
            for (size_t i = 0; i < chunk.row_count(); ++i)
                vals.push_back(chunk.column(col).get_f64(i));
        }
        return vals;
    }

    std::unique_ptr<Catalog> catalog_;
};

TEST_F(PlannerAggTest, CountStarPlanOk) {
    auto stmt = bind("SELECT count(*) FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
}

TEST_F(PlannerAggTest, CountStarValue) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT count(*) FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_i64(*op);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0], 3);
}

TEST_F(PlannerAggTest, SumValue) {
    insert({{Value{i32(1)}, Value{f64(0.0)}},
            {Value{i32(2)}, Value{f64(0.0)}},
            {Value{i32(3)}, Value{f64(0.0)}}});
    auto stmt = bind("SELECT sum(id) FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_i64(*op);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0], 6);
}

TEST_F(PlannerAggTest, AvgValue) {
    insert({{Value{i32(1)}, Value{f64(0.0)}},
            {Value{i32(2)}, Value{f64(0.0)}},
            {Value{i32(3)}, Value{f64(0.0)}}});
    auto stmt = bind("SELECT avg(id) FROM users");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_f64(*op);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_DOUBLE_EQ(vals[0], 2.0);
}

TEST_F(PlannerAggTest, GroupByCountStarRowCount) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT id, count(*) FROM users GROUP BY id");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 3u);
}

TEST_F(PlannerAggTest, GroupByCountStarValues) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(1)}, Value{f64(2.0)}},
            {Value{i32(2)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT id, count(*) FROM users GROUP BY id ORDER BY id");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto counts = drain_i64(*op, 1);
    ASSERT_EQ(counts.size(), 2u);
    EXPECT_EQ(counts[0], 2);
    EXPECT_EQ(counts[1], 1);
}

TEST_F(PlannerAggTest, HavingFiltersGroups) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(1)}, Value{f64(2.0)}},
            {Value{i32(2)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT id, count(*) FROM users GROUP BY id HAVING count(*) > 1");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 1u);
}

TEST_F(PlannerAggTest, OrderByAggregateAlias) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(1)}, Value{f64(2.0)}},
            {Value{i32(2)}, Value{f64(3.0)}},
            {Value{i32(2)}, Value{f64(4.0)}},
            {Value{i32(2)}, Value{f64(5.0)}}});
    auto stmt = bind("SELECT id, count(*) AS cnt FROM users GROUP BY id ORDER BY cnt DESC");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto counts = drain_i64(*op, 1);
    ASSERT_EQ(counts.size(), 2u);
    EXPECT_EQ(counts[0], 3);
    EXPECT_EQ(counts[1], 2);
}

TEST_F(PlannerAggTest, GroupByOnlyNoAggFunc) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(1)}, Value{f64(2.0)}},
            {Value{i32(2)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT id FROM users GROUP BY id");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 2u);
}

TEST_F(PlannerAggTest, LimitAfterAggregate) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}},
            {Value{i32(4)}, Value{f64(4.0)}}});
    auto stmt = bind("SELECT id, count(*) FROM users GROUP BY id LIMIT 2");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 2u);
}

TEST_F(PlannerAggTest, WhereBeforeAggregate) {
    insert({{Value{i32(1)}, Value{f64(1.0)}},
            {Value{i32(2)}, Value{f64(2.0)}},
            {Value{i32(3)}, Value{f64(3.0)}}});
    auto stmt = bind("SELECT count(*) FROM users WHERE id > 1");
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    auto vals = drain_i64(*op);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0], 2);
}
