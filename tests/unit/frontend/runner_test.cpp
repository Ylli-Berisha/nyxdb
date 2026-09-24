#include "binder/bound_ast.h"
#include "catalog/catalog.h"
#include "frontend/runner.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
using namespace nyx::bound;
using namespace nyx::frontend;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_runner_test";

class RunnerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(ROOT);
        fs::create_directories(ROOT);
        auto r = Catalog::load(ROOT);
        ASSERT_TRUE(r.is_ok()) << r.error().message;
        cat_ = std::make_unique<Catalog>(std::move(r.value()));
    }
    void TearDown() override {
        cat_.reset();
        fs::remove_all(ROOT);
    }

    static Schema two_col_schema() {
        return {
            {"id", TypeId::INT32, false, 0, std::nullopt},
            {"val", TypeId::VARCHAR, true, 32, std::nullopt},
        };
    }

    std::unique_ptr<Catalog> cat_;
};

// ---- run_create_table -------------------------------------------------------

TEST_F(RunnerTest, CreateTableAddsToSchema) {
    BoundCreateTable stmt{"users", two_col_schema(), {}};
    ASSERT_TRUE(run_create_table(*cat_, stmt).is_ok());
    EXPECT_TRUE(cat_->has_table("users"));
    EXPECT_EQ(cat_->table("users")->schema().size(), 2u);
}

TEST_F(RunnerTest, CreateTableDuplicateReturnsError) {
    BoundCreateTable stmt{"t", two_col_schema(), {}};
    ASSERT_TRUE(run_create_table(*cat_, stmt).is_ok());
    auto r = run_create_table(*cat_, stmt);
    EXPECT_FALSE(r.is_ok());
}

// ---- run_insert / run_delete ------------------------------------------------

TEST_F(RunnerTest, InsertAndRowCount) {
    BoundCreateTable ct{"orders", {{"id", TypeId::INT32, false, 0, std::nullopt}}, {}};
    ASSERT_TRUE(run_create_table(*cat_, ct).is_ok());

    BoundInsert ins{"orders", {{Value{i32(1)}}, {Value{i32(2)}}, {Value{i32(3)}}}};
    auto r = run_insert(*cat_, ins);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 3u);
}

TEST_F(RunnerTest, DeleteReducesRows) {
    BoundCreateTable ct{"del_t", {{"id", TypeId::INT32, false, 0, std::nullopt}}, {}};
    ASSERT_TRUE(run_create_table(*cat_, ct).is_ok());

    BoundInsert ins{"del_t", {{Value{i32(10)}}, {Value{i32(20)}}, {Value{i32(30)}}}};
    ASSERT_TRUE(run_insert(*cat_, ins).is_ok());

    Schema schema = {{"id", TypeId::INT32, false, 0, std::nullopt}};
    BoundDelete del{"del_t", schema, nullptr};
    auto r = run_delete(*cat_, del);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 3u);
}

// ---- run_drop_table ---------------------------------------------------------

TEST_F(RunnerTest, DropTableRemovesFromCatalog) {
    BoundCreateTable ct{"tmp", two_col_schema(), {}};
    ASSERT_TRUE(run_create_table(*cat_, ct).is_ok());
    ASSERT_TRUE(cat_->has_table("tmp"));

    BoundDropTable drop{"tmp", false};
    ASSERT_TRUE(run_drop_table(*cat_, drop).is_ok());
    EXPECT_FALSE(cat_->has_table("tmp"));
}

TEST_F(RunnerTest, DropTableNonExistentReturnsError) {
    BoundDropTable drop{"nope", false};
    EXPECT_FALSE(run_drop_table(*cat_, drop).is_ok());
}

TEST_F(RunnerTest, DropTableIfExistsNoError) {
    BoundDropTable drop{"nope", true};
    EXPECT_TRUE(run_drop_table(*cat_, drop).is_ok());
}

// ---- run_create_index / run_drop_index --------------------------------------

TEST_F(RunnerTest, CreateIndexAddsToMeta) {
    BoundCreateTable ct{"idx_t", {{"id", TypeId::INT32, false, 0, std::nullopt}}, {}};
    ASSERT_TRUE(run_create_table(*cat_, ct).is_ok());

    BoundCreateIndex ci{"idx_t", "idx_id", {0}, false};
    ASSERT_TRUE(run_create_index(*cat_, ci).is_ok());

    const auto& metas = cat_->indexes_of("idx_t");
    ASSERT_EQ(metas.size(), 1u);
    EXPECT_EQ(metas[0].name, "idx_id");
}

TEST_F(RunnerTest, DropIndexRemovesFromMeta) {
    BoundCreateTable ct{"drop_idx_t", {{"id", TypeId::INT32, false, 0, std::nullopt}}, {}};
    ASSERT_TRUE(run_create_table(*cat_, ct).is_ok());

    BoundCreateIndex ci{"drop_idx_t", "idx_id", {0}, false};
    ASSERT_TRUE(run_create_index(*cat_, ci).is_ok());

    BoundDropIndex di{"drop_idx_t", "idx_id"};
    ASSERT_TRUE(run_drop_index(*cat_, di).is_ok());
    EXPECT_TRUE(cat_->indexes_of("drop_idx_t").empty());
}

// ---- run_vacuum -------------------------------------------------------------

TEST_F(RunnerTest, VacuumOnEmptyTableSucceeds) {
    BoundCreateTable ct{"vac_t", {{"id", TypeId::INT32, false, 0, std::nullopt}}, {}};
    ASSERT_TRUE(run_create_table(*cat_, ct).is_ok());

    BoundVacuum vac{"vac_t"};
    EXPECT_TRUE(run_vacuum(*cat_, vac).is_ok());
}
