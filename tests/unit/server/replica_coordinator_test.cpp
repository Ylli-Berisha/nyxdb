#include "server/replica_coordinator.h"
#include "server/shard_client_pool.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace nyx;
using namespace nyx::server;

struct FakePool : public ShardClientPool {
    struct Call {
        std::string addr;
        std::string sql;
    };

    Result<ExecuteResult> execute(const std::string& addr, const std::string&,
                                  const std::string& sql) override {
        calls.push_back({addr, sql});
        ExecuteResult r;
        r.rows_affected = 0;
        return Result<ExecuteResult>::ok(std::move(r));
    }

    void evict(const std::string& addr) override { evicted.push_back(addr); }

    std::vector<Call> calls;
    std::vector<std::string> evicted;
};

static const std::string TOKEN = "rc-unit-token";
static const std::string A = "127.0.0.1:19001";
static const std::string B = "127.0.0.1:19002";
static const std::string C = "127.0.0.1:19003";

static ReplicaCoordinator make_rc(FakePool*& out_pool) {
    auto p = std::make_unique<FakePool>();
    out_pool = p.get();
    return ReplicaCoordinator(TOKEN, std::move(p));
}

TEST(ReplicaCoordinatorRegister, FirstNodeAsLeaderSetsLeader) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("INSERT INTO t VALUES (1)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorRegister, FirstNodeAsFollowerNoLeader) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);

    auto r = rc.execute("INSERT INTO t VALUES (1)");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no leader"), std::string::npos);
    EXPECT_TRUE(pool->calls.empty());
}

TEST(ReplicaCoordinatorRegister, FirstNodeAddedToReplicas) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);

    rc.execute("SELECT * FROM t");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorRegister, DuplicateAddressIgnoredInReplicas) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);
    rc.register_node(A, false);
    rc.register_node(A, false);

    for (int i = 0; i < 5; ++i)
        rc.execute("SELECT * FROM t");

    ASSERT_EQ(pool->calls.size(), 5u);
    for (const auto& c : pool->calls)
        EXPECT_EQ(c.addr, A);
}

TEST(ReplicaCoordinatorRegister, MultipleDistinctNodesAllInReplicas) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);
    rc.register_node(B, false);
    rc.register_node(C, false);

    for (int i = 0; i < 3; ++i)
        rc.execute("SELECT * FROM t");

    std::vector<std::string> addrs;
    for (const auto& c : pool->calls)
        addrs.push_back(c.addr);
    EXPECT_NE(std::find(addrs.begin(), addrs.end(), A), addrs.end());
    EXPECT_NE(std::find(addrs.begin(), addrs.end(), B), addrs.end());
    EXPECT_NE(std::find(addrs.begin(), addrs.end(), C), addrs.end());
}

TEST(ReplicaCoordinatorRegister, SecondRegisterOverridesLeader) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.register_node(B, true);

    rc.execute("INSERT INTO t VALUES (1)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, B);
}

TEST(ReplicaCoordinatorRegister, RegisterFollowerThenPromoteLeader) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);

    auto r = rc.execute("INSERT INTO t VALUES (1)");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no leader"), std::string::npos);
    EXPECT_TRUE(pool->calls.empty());

    rc.register_node(A, true);
    rc.execute("INSERT INTO t VALUES (1)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorNotify, UpdatesLeaderAddr) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.register_node(B, false);

    rc.notify_leader(A, C);

    rc.execute("INSERT INTO t VALUES (1)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, C);
}

TEST(ReplicaCoordinatorNotify, RemovesOldAddrFromReplicas) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.notify_leader(A, B);

    auto r = rc.execute("SELECT * FROM t");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no replicas"), std::string::npos);
    EXPECT_TRUE(pool->calls.empty());
}

TEST(ReplicaCoordinatorNotify, EvictsOldAddrFromPool) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.notify_leader(A, B);

    ASSERT_EQ(pool->evicted.size(), 1u);
    EXPECT_EQ(pool->evicted[0], A);
}

TEST(ReplicaCoordinatorNotify, NewLeaderRemainsInReplicasIfAlreadyRegistered) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.register_node(B, false);

    rc.notify_leader(A, B);

    rc.execute("SELECT * FROM t");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, B);
}

TEST(ReplicaCoordinatorNotify, UnknownOldAddrDoesNotCrash) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    EXPECT_NO_FATAL_FAILURE(rc.notify_leader("127.0.0.1:0", B));
}

TEST(ReplicaCoordinatorNotify, EvictsEvenIfOldAddrNotInReplicas) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.notify_leader("not-in-replicas:9999", B);

    ASSERT_EQ(pool->evicted.size(), 1u);
    EXPECT_EQ(pool->evicted[0], "not-in-replicas:9999");
}

TEST(ReplicaCoordinatorNotify, SequentialFailoversUpdateLeader) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.register_node(B, false);
    rc.register_node(C, false);

    rc.notify_leader(A, B);
    rc.notify_leader(B, C);

    rc.execute("INSERT INTO t VALUES (1)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, C);
}

TEST(ReplicaCoordinatorIsWrite, SelectIsRead) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("SELECT * FROM t");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorIsWrite, SelectLeadingWhitespace) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("   SELECT * FROM t");
    ASSERT_EQ(pool->calls.size(), 1u);
}

TEST(ReplicaCoordinatorIsWrite, SelectLowercase) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("select * from t");
    ASSERT_EQ(pool->calls.size(), 1u);
}

TEST(ReplicaCoordinatorIsWrite, SelectMixedCase) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("SeLeCt * FROM t");
    ASSERT_EQ(pool->calls.size(), 1u);
}

TEST(ReplicaCoordinatorIsWrite, InsertIsWrite) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("INSERT INTO t VALUES (1)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorIsWrite, UpdateIsWrite) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.register_node(B, false);

    rc.execute("UPDATE t SET x=1");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorIsWrite, DeleteIsWrite) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.register_node(B, false);

    rc.execute("DELETE FROM t WHERE id=1");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorIsWrite, CreateIsWrite) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("CREATE TABLE t (id INT NOT NULL)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorIsWrite, DropIsWrite) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("DROP TABLE t");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].addr, A);
}

TEST(ReplicaCoordinatorIsWrite, ShortSqlIsWrite) {
    FakePool* pool;
    auto rc = make_rc(pool);

    auto r = rc.execute("SEL");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no leader"), std::string::npos);
}

TEST(ReplicaCoordinatorIsWrite, EmptySqlIsWrite) {
    FakePool* pool;
    auto rc = make_rc(pool);

    auto r = rc.execute("");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no leader"), std::string::npos);
}

TEST(ReplicaCoordinatorExecute, ReadNoReplicasError) {
    FakePool* pool;
    auto rc = make_rc(pool);

    auto r = rc.execute("SELECT * FROM t");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no replicas"), std::string::npos);
    EXPECT_TRUE(pool->calls.empty());
}

TEST(ReplicaCoordinatorExecute, WriteNoLeaderError) {
    FakePool* pool;
    auto rc = make_rc(pool);

    auto r = rc.execute("INSERT INTO t VALUES (1)");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no leader"), std::string::npos);
    EXPECT_TRUE(pool->calls.empty());
}

TEST(ReplicaCoordinatorExecute, WriteNoLeaderEvenWithReplicasRegistered) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);
    rc.register_node(B, false);

    auto r = rc.execute("INSERT INTO t VALUES (1)");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no leader"), std::string::npos);
    EXPECT_TRUE(pool->calls.empty());
}

TEST(ReplicaCoordinatorExecute, ReadAfterNotifyRemovesLastReplicaIsNoReplicas) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.notify_leader(A, B);

    auto r = rc.execute("SELECT * FROM t");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(r.error().message.find("no replicas"), std::string::npos);
}

TEST(ReplicaCoordinatorExecute, WriteSqlPassedThroughToPool) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);

    rc.execute("INSERT INTO t VALUES (42)");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].sql, "INSERT INTO t VALUES (42)");
}

TEST(ReplicaCoordinatorExecute, ReadSqlPassedThroughToPool) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);

    rc.execute("SELECT id FROM t WHERE id > 5");
    ASSERT_EQ(pool->calls.size(), 1u);
    EXPECT_EQ(pool->calls[0].sql, "SELECT id FROM t WHERE id > 5");
}

TEST(ReplicaCoordinatorRoundRobin, TwoReplicasCycleEvenly) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);
    rc.register_node(B, false);

    for (int i = 0; i < 6; ++i)
        rc.execute("SELECT * FROM t");

    ASSERT_EQ(pool->calls.size(), 6u);
    int a_count = 0, b_count = 0;
    for (const auto& c : pool->calls) {
        if (c.addr == A)
            ++a_count;
        if (c.addr == B)
            ++b_count;
    }
    EXPECT_EQ(a_count, 3);
    EXPECT_EQ(b_count, 3);
}

TEST(ReplicaCoordinatorRoundRobin, ThreeReplicasCycleInOrder) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);
    rc.register_node(B, false);
    rc.register_node(C, false);

    for (int i = 0; i < 6; ++i)
        rc.execute("SELECT * FROM t");

    ASSERT_EQ(pool->calls.size(), 6u);
    EXPECT_EQ(pool->calls[0].addr, A);
    EXPECT_EQ(pool->calls[1].addr, B);
    EXPECT_EQ(pool->calls[2].addr, C);
    EXPECT_EQ(pool->calls[3].addr, A);
    EXPECT_EQ(pool->calls[4].addr, B);
    EXPECT_EQ(pool->calls[5].addr, C);
}

TEST(ReplicaCoordinatorRoundRobin, SingleReplicaAlwaysUsed) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);

    for (int i = 0; i < 5; ++i)
        rc.execute("SELECT * FROM t");

    ASSERT_EQ(pool->calls.size(), 5u);
    for (const auto& c : pool->calls)
        EXPECT_EQ(c.addr, A);
}

TEST(ReplicaCoordinatorRoundRobin, WritesAlwaysGoToLeaderNotReplicas) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);
    rc.register_node(B, true);
    rc.register_node(C, false);

    for (int i = 0; i < 6; ++i)
        rc.execute("INSERT INTO t VALUES (" + std::to_string(i) + ")");

    ASSERT_EQ(pool->calls.size(), 6u);
    for (const auto& c : pool->calls)
        EXPECT_EQ(c.addr, B);
}

TEST(ReplicaCoordinatorRoundRobin, RoundRobinContinuesAfterNotify) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, true);
    rc.register_node(B, false);
    rc.register_node(C, false);

    rc.notify_leader(A, B);

    for (int i = 0; i < 4; ++i)
        rc.execute("SELECT * FROM t");

    ASSERT_EQ(pool->calls.size(), 4u);
    EXPECT_EQ(pool->calls[0].addr, B);
    EXPECT_EQ(pool->calls[1].addr, C);
    EXPECT_EQ(pool->calls[2].addr, B);
    EXPECT_EQ(pool->calls[3].addr, C);
}

TEST(ReplicaCoordinatorRoundRobin, NoDuplicateAfterDuplicateRegister) {
    FakePool* pool;
    auto rc = make_rc(pool);
    rc.register_node(A, false);
    rc.register_node(A, false);
    rc.register_node(B, false);

    for (int i = 0; i < 4; ++i)
        rc.execute("SELECT * FROM t");

    ASSERT_EQ(pool->calls.size(), 4u);
    EXPECT_EQ(pool->calls[0].addr, A);
    EXPECT_EQ(pool->calls[1].addr, B);
    EXPECT_EQ(pool->calls[2].addr, A);
    EXPECT_EQ(pool->calls[3].addr, B);
}
