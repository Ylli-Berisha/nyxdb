#include "replication/follower_registry.h"

#include <gtest/gtest.h>
#include <limits>

using namespace nyx;
using namespace nyx::replication;

TEST(FollowerRegistryTest, EmptyMinLsnIsMaxU64) {
    FollowerRegistry reg;
    EXPECT_EQ(reg.min_confirmed_lsn(), std::numeric_limits<u64>::max());
}

TEST(FollowerRegistryTest, UpsertAndMinLsn) {
    FollowerRegistry reg;
    reg.upsert("node1", 100, 0);
    EXPECT_EQ(reg.min_confirmed_lsn(), 100u);
}

TEST(FollowerRegistryTest, MinLsnAcrossMultiple) {
    FollowerRegistry reg;
    reg.upsert("n1", 200, 0);
    reg.upsert("n2", 50, 0);
    reg.upsert("n3", 300, 0);
    EXPECT_EQ(reg.min_confirmed_lsn(), 50u);
}

TEST(FollowerRegistryTest, UpsertUpdatesExisting) {
    FollowerRegistry reg;
    reg.upsert("n1", 10, 0);
    reg.upsert("n1", 99, 0);
    EXPECT_EQ(reg.min_confirmed_lsn(), 99u);
}

TEST(FollowerRegistryTest, All) {
    FollowerRegistry reg;
    reg.upsert("a", 1, 2);
    reg.upsert("b", 3, 0);
    auto all = reg.all();
    EXPECT_EQ(all.size(), 2u);
}

TEST(FollowerRegistryTest, EvictStaleMarksFollower) {
    FollowerRegistry reg;
    reg.upsert("slow", 0, 0);
    reg.upsert("fast", 900, 0);

    // leader at lsn 1000, max lag 500 bytes → "slow" (lag=1000) is stale
    reg.evict_stale(1000, 500);

    // stale followers are excluded from min_confirmed_lsn
    EXPECT_EQ(reg.min_confirmed_lsn(), 900u);
}

TEST(FollowerRegistryTest, EvictStaleDoesNotMarkWithinThreshold) {
    FollowerRegistry reg;
    reg.upsert("n1", 800, 0);
    reg.evict_stale(1000, 500); // lag=200 < 500 → not stale
    EXPECT_EQ(reg.min_confirmed_lsn(), 800u);
}

TEST(FollowerRegistryTest, StaleNotCountedInMin) {
    FollowerRegistry reg;
    reg.upsert("stale_node", 0, 0);
    reg.evict_stale(1000, 500);
    // only stale follower — min returns max u64 (no active followers)
    EXPECT_EQ(reg.min_confirmed_lsn(), std::numeric_limits<u64>::max());
}

TEST(FollowerRegistryTest, UpsertClearsStaleFlag) {
    FollowerRegistry reg;
    reg.upsert("n1", 0, 0);
    reg.evict_stale(1000, 500);
    EXPECT_EQ(reg.min_confirmed_lsn(), std::numeric_limits<u64>::max());

    // re-upsert clears stale flag
    reg.upsert("n1", 950, 0);
    EXPECT_EQ(reg.min_confirmed_lsn(), 950u);
}
