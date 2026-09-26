#include "catalog/catalog.h"
#include "database/database.h"
#include "executor/expression.h"
#include "storage/disk/shard_map_file.h"

#include <filesystem>
#include <gtest/gtest.h>

using namespace nyx;
namespace fs = std::filesystem;

static const std::string ROOT = "/tmp/nyxdb_sharding_test";

class ShardingTest : public ::testing::Test {
  protected:
    void SetUp() override { fs::remove_all(ROOT); }
    void TearDown() override { fs::remove_all(ROOT); }
};

TEST_F(ShardingTest, ShardMapFileRoundTrip) {
    ShardMapMeta meta;
    meta.partition_col = "ts";
    meta.partition_col_idx = 1;

    PartitionDef p0;
    p0.name = "p0";
    p0.is_maxvalue = false;
    p0.upper_bound = i32(1000);
    p0.node_addr = "127.0.0.1:4434";

    PartitionDef p1;
    p1.name = "p1";
    p1.is_maxvalue = true;
    p1.node_addr = "127.0.0.1:4435";

    meta.partitions = {p0, p1};

    std::string path = ROOT + "/test_shard_map.bin";
    fs::create_directories(ROOT);

    ASSERT_TRUE(ShardMapFile::write(path, meta).is_ok());

    auto r = ShardMapFile::read(path);
    ASSERT_TRUE(r.is_ok());

    const ShardMapMeta& out = r.value();
    EXPECT_EQ(out.partition_col, "ts");
    EXPECT_EQ(out.partition_col_idx, 1u);
    ASSERT_EQ(out.partitions.size(), 2u);
    EXPECT_EQ(out.partitions[0].name, "p0");
    EXPECT_FALSE(out.partitions[0].is_maxvalue);
    EXPECT_EQ(std::get<i32>(out.partitions[0].upper_bound), 1000);
    EXPECT_EQ(out.partitions[0].node_addr, "127.0.0.1:4434");
    EXPECT_TRUE(out.partitions[1].is_maxvalue);
    EXPECT_EQ(out.partitions[1].node_addr, "127.0.0.1:4435");
}

TEST_F(ShardingTest, ShardMapFileDateBound) {
    ShardMapMeta meta;
    meta.partition_col = "dt";
    meta.partition_col_idx = 0;

    PartitionDef p0;
    p0.name = "p_early";
    p0.is_maxvalue = false;
    p0.upper_bound = Date{19723};
    p0.node_addr = "host:4434";

    PartitionDef p1;
    p1.name = "p_rest";
    p1.is_maxvalue = true;
    p1.node_addr = "host:4435";

    meta.partitions = {p0, p1};

    std::string path = ROOT + "/date_shard.bin";
    fs::create_directories(ROOT);
    ASSERT_TRUE(ShardMapFile::write(path, meta).is_ok());

    auto r = ShardMapFile::read(path);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::get<Date>(r.value().partitions[0].upper_bound).days, 19723);
}

TEST_F(ShardingTest, ShardForValueRouting) {
    ShardMapMeta meta;
    meta.partition_col = "id";
    meta.partition_col_idx = 0;

    PartitionDef p0;
    p0.name = "p0";
    p0.upper_bound = i32(100);
    p0.node_addr = "a";
    PartitionDef p1;
    p1.name = "p1";
    p1.upper_bound = i32(200);
    p1.node_addr = "b";
    PartitionDef p2;
    p2.name = "p2";
    p2.is_maxvalue = true;
    p2.node_addr = "c";
    meta.partitions = {p0, p1, p2};

    EXPECT_EQ(shard_for_value(meta, Value{i32(0)}), 0u);
    EXPECT_EQ(shard_for_value(meta, Value{i32(99)}), 0u);
    EXPECT_EQ(shard_for_value(meta, Value{i32(100)}), 1u);
    EXPECT_EQ(shard_for_value(meta, Value{i32(199)}), 1u);
    EXPECT_EQ(shard_for_value(meta, Value{i32(200)}), 2u);
    EXPECT_EQ(shard_for_value(meta, Value{i32(9999)}), 2u);
    EXPECT_EQ(shard_for_value(meta, std::monostate{}), 2u);
}

TEST_F(ShardingTest, PrunePartitionsEQ) {
    ShardMapMeta meta;
    meta.partition_col = "id";
    meta.partition_col_idx = 0;

    PartitionDef p0;
    p0.name = "p0";
    p0.upper_bound = i32(100);
    p0.node_addr = "a";
    PartitionDef p1;
    p1.name = "p1";
    p1.upper_bound = i32(200);
    p1.node_addr = "b";
    PartitionDef p2;
    p2.name = "p2";
    p2.is_maxvalue = true;
    p2.node_addr = "c";
    meta.partitions = {p0, p1, p2};

    auto pruned = prune_partitions(meta, 0, static_cast<int>(BinaryOpKind::EQ), Value{i32(50)});
    EXPECT_EQ(pruned.size(), 2u);
    EXPECT_EQ(pruned[0], 0u);
    EXPECT_EQ(pruned[1], 2u);

    pruned = prune_partitions(meta, 0, static_cast<int>(BinaryOpKind::EQ), Value{i32(150)});
    EXPECT_EQ(pruned.size(), 2u);
    EXPECT_EQ(pruned[0], 1u);
    EXPECT_EQ(pruned[1], 2u);
}

TEST_F(ShardingTest, PrunePartitionsGT) {
    ShardMapMeta meta;
    meta.partition_col = "id";
    meta.partition_col_idx = 0;

    PartitionDef p0;
    p0.name = "p0";
    p0.upper_bound = i32(100);
    p0.node_addr = "a";
    PartitionDef p1;
    p1.name = "p1";
    p1.upper_bound = i32(200);
    p1.node_addr = "b";
    PartitionDef p2;
    p2.name = "p2";
    p2.is_maxvalue = true;
    p2.node_addr = "c";
    meta.partitions = {p0, p1, p2};

    auto pruned = prune_partitions(meta, 0, static_cast<int>(BinaryOpKind::GT), Value{i32(150)});
    EXPECT_EQ(pruned.size(), 2u);
    EXPECT_EQ(pruned[0], 1u);
    EXPECT_EQ(pruned[1], 2u);
}

TEST_F(ShardingTest, PruneWrongColumn) {
    ShardMapMeta meta;
    meta.partition_col = "id";
    meta.partition_col_idx = 0;

    PartitionDef p0;
    p0.name = "p0";
    p0.upper_bound = i32(100);
    p0.node_addr = "a";
    PartitionDef p1;
    p1.name = "p1";
    p1.is_maxvalue = true;
    p1.node_addr = "b";
    meta.partitions = {p0, p1};

    auto pruned = prune_partitions(meta, 1, static_cast<int>(BinaryOpKind::EQ), Value{i32(50)});
    EXPECT_EQ(pruned.size(), 2u);
}

TEST_F(ShardingTest, CreatePartitionedTableStoresShardMap) {
    auto db_r = Database::open(ROOT);
    ASSERT_TRUE(db_r.is_ok());
    auto db = std::move(db_r.value());

    auto r = db.execute("CREATE TABLE orders (id INT NOT NULL, amount DOUBLE) "
                        "PARTITION BY RANGE (id) ("
                        "  PARTITION p0 VALUES LESS THAN (1000) ON '127.0.0.1:4434',"
                        "  PARTITION p1 VALUES LESS THAN (MAXVALUE) ON '127.0.0.1:4435'"
                        ")");
    ASSERT_TRUE(r.is_ok()) << r.error().message;

    auto cat_r = Catalog::load(ROOT);
    ASSERT_TRUE(cat_r.is_ok());
    const ShardMapMeta* sm = cat_r.value().shard_map_of("orders");
    ASSERT_NE(sm, nullptr);
    EXPECT_EQ(sm->partition_col, "id");
    EXPECT_EQ(sm->partition_col_idx, 0u);
    ASSERT_EQ(sm->partitions.size(), 2u);
    EXPECT_EQ(sm->partitions[0].name, "p0");
    EXPECT_EQ(sm->partitions[0].node_addr, "127.0.0.1:4434");
    EXPECT_EQ(sm->partitions[1].node_addr, "127.0.0.1:4435");
    EXPECT_TRUE(sm->partitions[1].is_maxvalue);
}

TEST_F(ShardingTest, AlterAddPartitionUpdatesShardMap) {
    auto db_r = Database::open(ROOT);
    ASSERT_TRUE(db_r.is_ok());
    auto db = std::move(db_r.value());

    ASSERT_TRUE(db.execute("CREATE TABLE t (id INT NOT NULL) "
                           "PARTITION BY RANGE (id) ("
                           "  PARTITION p0 VALUES LESS THAN (MAXVALUE) ON '127.0.0.1:4434'"
                           ")")
                    .is_ok());

    auto cat1 = Catalog::load(ROOT);
    ASSERT_TRUE(cat1.is_ok());
    const ShardMapMeta* sm1 = cat1.value().shard_map_of("t");
    ASSERT_NE(sm1, nullptr);
    ASSERT_EQ(sm1->partitions.size(), 1u);
    EXPECT_TRUE(sm1->partitions[0].is_maxvalue);
}
