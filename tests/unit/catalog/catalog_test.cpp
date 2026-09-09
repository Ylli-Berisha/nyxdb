#include "catalog/catalog.h"
#include "storage/disk/table.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string TEST_ROOT = "/tmp/nyxdb_catalog_test";

class CatalogTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(TEST_ROOT);
        fs::create_directories(TEST_ROOT);
    }
    void TearDown() override { fs::remove_all(TEST_ROOT); }

    static Schema simple_schema() {
        return Schema{
            Column{"id", TypeId::INT32, false},
            Column{"name", TypeId::INT64, true},
        };
    }
};

TEST_F(CatalogTest, LoadMissingRootIsEmpty) {
    fs::remove_all(TEST_ROOT);
    auto r = Catalog::load(TEST_ROOT);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().size(), 0u);
}

TEST_F(CatalogTest, LoadEmptyRootIsEmpty) {
    auto r = Catalog::load(TEST_ROOT);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().size(), 0u);
}

TEST_F(CatalogTest, LoadWithPreexistingTables) {
    {
        auto t1 = Table::create(TEST_ROOT, "users", simple_schema());
        ASSERT_TRUE(t1.is_ok()) << t1.error().message;
        ASSERT_TRUE(t1.value().flush().is_ok());
        auto t2 = Table::create(TEST_ROOT, "orders", Schema{Column{"id", TypeId::INT64, false}});
        ASSERT_TRUE(t2.is_ok()) << t2.error().message;
        ASSERT_TRUE(t2.value().flush().is_ok());
    }
    auto r = Catalog::load(TEST_ROOT);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(r.value().size(), 2u);
    EXPECT_TRUE(r.value().has_table("users"));
    EXPECT_TRUE(r.value().has_table("orders"));
}

TEST_F(CatalogTest, AddTableRegisters) {
    auto r = Catalog::load(TEST_ROOT);
    ASSERT_TRUE(r.is_ok());
    Catalog cat = std::move(r.value());
    ASSERT_TRUE(cat.add_table("users", simple_schema()).is_ok());
    EXPECT_TRUE(cat.has_table("users"));
    EXPECT_EQ(cat.size(), 1u);
}

TEST_F(CatalogTest, AddTableDuplicateErrors) {
    auto r = Catalog::load(TEST_ROOT);
    Catalog cat = std::move(r.value());
    ASSERT_TRUE(cat.add_table("t", simple_schema()).is_ok());
    auto second = cat.add_table("t", simple_schema());
    ASSERT_TRUE(second.is_err());
    EXPECT_NE(second.error().message.find("already exists"), std::string::npos);
}

TEST_F(CatalogTest, LookupIsCaseInsensitive) {
    auto r = Catalog::load(TEST_ROOT);
    Catalog cat = std::move(r.value());
    ASSERT_TRUE(cat.add_table("Users", simple_schema()).is_ok());
    EXPECT_TRUE(cat.has_table("users"));
    EXPECT_TRUE(cat.has_table("USERS"));
    EXPECT_TRUE(cat.has_table("Users"));
}

TEST_F(CatalogTest, AddCaseInsensitiveDuplicateErrors) {
    auto r = Catalog::load(TEST_ROOT);
    Catalog cat = std::move(r.value());
    ASSERT_TRUE(cat.add_table("Users", simple_schema()).is_ok());
    auto second = cat.add_table("USERS", simple_schema());
    ASSERT_TRUE(second.is_err());
}

TEST_F(CatalogTest, SchemaOfReturnsSchema) {
    auto r = Catalog::load(TEST_ROOT);
    Catalog cat = std::move(r.value());
    ASSERT_TRUE(cat.add_table("t", simple_schema()).is_ok());
    const Schema* s = cat.schema_of("t");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->size(), 2u);
    EXPECT_EQ((*s)[0].name, "id");
    EXPECT_EQ((*s)[0].type, TypeId::INT32);
    EXPECT_EQ((*s)[1].name, "name");
    EXPECT_EQ((*s)[1].type, TypeId::INT64);
}

TEST_F(CatalogTest, SchemaOfMissingReturnsNullptr) {
    auto r = Catalog::load(TEST_ROOT);
    Catalog cat = std::move(r.value());
    EXPECT_EQ(cat.schema_of("ghost"), nullptr);
    EXPECT_EQ(cat.table("ghost"), nullptr);
}

TEST_F(CatalogTest, TableAccessorAllowsInsert) {
    auto r = Catalog::load(TEST_ROOT);
    Catalog cat = std::move(r.value());
    ASSERT_TRUE(cat.add_table("t", Schema{Column{"id", TypeId::INT32, false}}).is_ok());
    Table* t = cat.table("t");
    ASSERT_NE(t, nullptr);
    auto ins = t->insert(std::vector<Value>{i32{42}});
    ASSERT_TRUE(ins.is_ok()) << ins.error().message;
    EXPECT_EQ(t->row_count(), 1u);
}

TEST_F(CatalogTest, ReloadSeesAddedTables) {
    {
        auto r = Catalog::load(TEST_ROOT);
        Catalog cat = std::move(r.value());
        ASSERT_TRUE(cat.add_table("t", simple_schema()).is_ok());
    }
    auto r2 = Catalog::load(TEST_ROOT);
    ASSERT_TRUE(r2.is_ok()) << r2.error().message;
    EXPECT_EQ(r2.value().size(), 1u);
    EXPECT_TRUE(r2.value().has_table("t"));
}
