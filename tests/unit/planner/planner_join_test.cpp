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

static const std::string ROOT = "/tmp/nyxdb_planner_join_test";

class PlannerJoinTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
        auto cat = Catalog::load(ROOT);
        ASSERT_TRUE(cat.is_ok());
        catalog_ = std::make_unique<Catalog>(std::move(cat.value()));

        Schema users{Column{"id", TypeId::INT32, false}, Column{"score", TypeId::DOUBLE, true}};
        Schema orders{Column{"order_id", TypeId::INT64, false},
                      Column{"user_id", TypeId::INT32, false}};
        Schema items{Column{"item_id", TypeId::INT32, false},
                     Column{"order_id", TypeId::INT64, false}};
        ASSERT_TRUE(catalog_->add_table("users", users).is_ok());
        ASSERT_TRUE(catalog_->add_table("orders", orders).is_ok());
        ASSERT_TRUE(catalog_->add_table("items", items).is_ok());
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

    void insert_users(std::vector<std::vector<Value>> rows) {
        ASSERT_TRUE(catalog_->table("users")->insert_many(rows).is_ok());
    }
    void insert_orders(std::vector<std::vector<Value>> rows) {
        ASSERT_TRUE(catalog_->table("orders")->insert_many(rows).is_ok());
    }
    void insert_items(std::vector<std::vector<Value>> rows) {
        ASSERT_TRUE(catalog_->table("items")->insert_many(rows).is_ok());
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

    std::unique_ptr<Catalog> catalog_;
};

static const std::string JOIN_SRC =
    "SELECT users.id FROM users JOIN orders ON users.id = orders.user_id";

TEST_F(PlannerJoinTest, TwoTableJoinReturnsOk) {
    auto stmt = bind(JOIN_SRC);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
}

TEST_F(PlannerJoinTest, JoinOutputColumnCount) {
    auto stmt = bind(JOIN_SRC);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->output_schema().size(), 1u);
}

TEST_F(PlannerJoinTest, JoinProducesMatchingRows) {
    insert_users({{Value{i32(1)}, Value{f64(1.0)}}, {Value{i32(2)}, Value{f64(2.0)}}});
    insert_orders({{Value{i64(10)}, Value{i32(1)}}, {Value{i64(11)}, Value{i32(2)}}});

    auto stmt = bind(JOIN_SRC);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 2u);
}

TEST_F(PlannerJoinTest, JoinFiltersNonMatchingRows) {
    insert_users({{Value{i32(1)}, Value{f64(1.0)}}});
    insert_orders({{Value{i64(10)}, Value{i32(99)}}});

    auto stmt = bind(JOIN_SRC);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok());
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 0u);
}

TEST_F(PlannerJoinTest, ReorderSmallLeftIsBuildSide) {
    // users has 1 row (smaller) → build side; 1 user matches 2 orders → 2 result rows
    insert_users({{Value{i32(42)}, Value{f64(1.0)}}});
    insert_orders({{Value{i64(10)}, Value{i32(42)}}, {Value{i64(11)}, Value{i32(42)}}});

    auto stmt = bind(JOIN_SRC);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 2u);
}

TEST_F(PlannerJoinTest, ReorderSmallRightIsBuildSide) {
    // orders has 1 row (smaller) → build side; only 1 of 2 users matches → 1 result row
    insert_users({{Value{i32(1)}, Value{f64(1.0)}}, {Value{i32(2)}, Value{f64(2.0)}}});
    insert_orders({{Value{i64(10)}, Value{i32(1)}}});

    auto stmt = bind(JOIN_SRC);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    auto op = std::move(r.value());
    EXPECT_EQ(drain(*op), 1u);
}

TEST_F(PlannerJoinTest, NonEquijoinErrors) {
    std::string src = "SELECT users.id FROM users JOIN orders ON users.id > orders.user_id";
    auto stmt = bind(src);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_err());
    EXPECT_NE(r.error().message.find("equijoin"), std::string::npos);
}

TEST_F(PlannerJoinTest, ThreeTableJoinReturnsOk) {
    std::string src = "SELECT users.id FROM users "
                      "JOIN orders ON users.id = orders.user_id "
                      "JOIN items ON orders.order_id = items.order_id";
    auto stmt = bind(src);
    Planner pl(*catalog_);
    auto r = pl.plan(stmt);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value()->output_schema().size(), 1u);
}
