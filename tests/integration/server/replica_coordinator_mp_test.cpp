#include "query_client.h"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace nyx;
using namespace nyx::server;

static constexpr u16 S0L_PORT = 15475;
static constexpr u16 S0F_PORT = 15476;
static constexpr u16 S1L_PORT = 15477;
static constexpr u16 S1F_PORT = 15478;
static constexpr u16 RC0_PORT = 15479;
static constexpr u16 RC1_PORT = 15480;
static constexpr u16 COORD_PORT = 15481;

static const std::string TOKEN = "rc-test-token";

static const std::string S0L_ROOT = "/tmp/nyxdb_rc_s0l";
static const std::string S0F_ROOT = "/tmp/nyxdb_rc_s0f";
static const std::string S1L_ROOT = "/tmp/nyxdb_rc_s1l";
static const std::string S1F_ROOT = "/tmp/nyxdb_rc_s1f";
static const std::string RC0_ROOT = "/tmp/nyxdb_rc_rc0";
static const std::string RC1_ROOT = "/tmp/nyxdb_rc_rc1";
static const std::string COORD_ROOT = "/tmp/nyxdb_rc_coord";

static const std::string S0_PEERS = "127.0.0.1:15475,127.0.0.2:15476";
static const std::string S1_PEERS = "127.0.0.3:15477,127.0.0.4:15478";
static const std::string COORD_ADDR = "127.0.0.1:15481";
static const std::string RC0_ADDR = "127.0.0.1:15479";
static const std::string RC1_ADDR = "127.0.0.1:15480";

struct RCProcess {
    pid_t pid = -1;
    u16 port = 0;
    std::string data_dir;

    RCProcess() = default;
    RCProcess(const RCProcess&) = delete;
    RCProcess& operator=(const RCProcess&) = delete;
    RCProcess(RCProcess&& o) noexcept : pid(o.pid), port(o.port), data_dir(std::move(o.data_dir)) {
        o.pid = -1;
    }
    ~RCProcess() { stop(); }

    static RCProcess spawn(const std::string& data_dir, u16 port, const std::string& token,
                           const std::string& role, const std::string& node_id = "",
                           const std::string& peers = "", const std::string& leader_addr = "",
                           const std::string& coordinator_addr = "",
                           const std::string& read_coordinator_addr = "") {
        RCProcess sp;
        sp.port = port;
        sp.data_dir = data_dir;
        fs::create_directories(data_dir);

        pid_t child = ::fork();
        if (child == 0) {
            std::vector<std::string> args = {
                NYXDB_SERVER_BIN,     "--data-dir",  data_dir, "--token", token, "--port",
                std::to_string(port), "--log-level", "error",  "--role",  role,
            };
            if (!node_id.empty()) {
                args.push_back("--node-id");
                args.push_back(node_id);
            }
            if (!peers.empty()) {
                args.push_back("--peers");
                args.push_back(peers);
            }
            if (!leader_addr.empty()) {
                args.push_back("--leader-addr");
                args.push_back(leader_addr);
            }
            if (!coordinator_addr.empty()) {
                args.push_back("--coordinator-addr");
                args.push_back(coordinator_addr);
            }
            if (!read_coordinator_addr.empty()) {
                args.push_back("--read-coordinator-addr");
                args.push_back(read_coordinator_addr);
            }

            std::vector<char*> argv_ptrs;
            for (auto& s : args)
                argv_ptrs.push_back(s.data());
            argv_ptrs.push_back(nullptr);

            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }
            ::execv(NYXDB_SERVER_BIN, argv_ptrs.data());
            ::_exit(1);
        }
        sp.pid = child;
        return sp;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int st;
            ::waitpid(pid, &st, 0);
            pid = -1;
        }
    }

    void crash() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int st;
            ::waitpid(pid, &st, 0);
            pid = -1;
        }
    }
};

static bool wait_ready(u16 port, const std::string& token,
                       std::chrono::seconds timeout = std::chrono::seconds(10)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QueryClient c;
        if (c.connect(port) && c.auth(token))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

static bool wait_replicated(u16 port, const std::string& sql, i64 expected,
                            std::chrono::seconds timeout = std::chrono::seconds(10)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QueryClient c;
        if (c.connect(port) && c.auth(TOKEN)) {
            if (c.query_row_count(sql) == expected)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static bool wait_write_ok(u16 port, const std::string& sql,
                          std::chrono::seconds timeout = std::chrono::seconds(15)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QueryClient c;
        if (c.connect(port) && c.auth(TOKEN) && c.exec(sql) >= 0)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

static std::string partitioned_sql(const std::string& tname) {
    return "CREATE TABLE " + tname +
           " (id INT NOT NULL, val INT NOT NULL)"
           " PARTITION BY RANGE (id)"
           " (PARTITION p0 VALUES LESS THAN (100) ON '" +
           RC0_ADDR +
           "',"
           "  PARTITION p1 VALUES LESS THAN (MAXVALUE) ON '" +
           RC1_ADDR + "')";
}

class ReplicaCoordinatorMpTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        for (const auto& d :
             {S0L_ROOT, S0F_ROOT, S1L_ROOT, S1F_ROOT, RC0_ROOT, RC1_ROOT, COORD_ROOT})
            fs::remove_all(d);

        rc0_ = std::make_unique<RCProcess>(
            RCProcess::spawn(RC0_ROOT, RC0_PORT, TOKEN, "read-coordinator"));
        ASSERT_GT(rc0_->pid, 0);
        ASSERT_TRUE(wait_ready(RC0_PORT, TOKEN)) << "read-coordinator-0 did not start";

        rc1_ = std::make_unique<RCProcess>(
            RCProcess::spawn(RC1_ROOT, RC1_PORT, TOKEN, "read-coordinator"));
        ASSERT_GT(rc1_->pid, 0);
        ASSERT_TRUE(wait_ready(RC1_PORT, TOKEN)) << "read-coordinator-1 did not start";

        s0l_ = std::make_unique<RCProcess>(RCProcess::spawn(
            S0L_ROOT, S0L_PORT, TOKEN, "leader", "127.0.0.1", S0_PEERS, "", COORD_ADDR, RC0_ADDR));
        ASSERT_GT(s0l_->pid, 0);
        ASSERT_TRUE(wait_ready(S0L_PORT, TOKEN)) << "shard0-leader did not start";

        s0f_ = std::make_unique<RCProcess>(
            RCProcess::spawn(S0F_ROOT, S0F_PORT, TOKEN, "follower", "127.0.0.2", S0_PEERS,
                             "127.0.0.1:" + std::to_string(S0L_PORT), "", RC0_ADDR));
        ASSERT_GT(s0f_->pid, 0);
        ASSERT_TRUE(wait_ready(S0F_PORT, TOKEN)) << "shard0-follower did not start";

        s1l_ = std::make_unique<RCProcess>(RCProcess::spawn(
            S1L_ROOT, S1L_PORT, TOKEN, "leader", "127.0.0.3", S1_PEERS, "", COORD_ADDR, RC1_ADDR));
        ASSERT_GT(s1l_->pid, 0);
        ASSERT_TRUE(wait_ready(S1L_PORT, TOKEN)) << "shard1-leader did not start";

        s1f_ = std::make_unique<RCProcess>(
            RCProcess::spawn(S1F_ROOT, S1F_PORT, TOKEN, "follower", "127.0.0.4", S1_PEERS,
                             "127.0.0.3:" + std::to_string(S1L_PORT), "", RC1_ADDR));
        ASSERT_GT(s1f_->pid, 0);
        ASSERT_TRUE(wait_ready(S1F_PORT, TOKEN)) << "shard1-follower did not start";

        coord_ = std::make_unique<RCProcess>(
            RCProcess::spawn(COORD_ROOT, COORD_PORT, TOKEN, "coordinator"));
        ASSERT_GT(coord_->pid, 0);
        ASSERT_TRUE(wait_ready(COORD_PORT, TOKEN)) << "coordinator did not start";

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    static void TearDownTestSuite() {
        coord_.reset();
        s1f_.reset();
        s1l_.reset();
        s0f_.reset();
        s0l_.reset();
        rc1_.reset();
        rc0_.reset();
        for (const auto& d :
             {S0L_ROOT, S0F_ROOT, S1L_ROOT, S1F_ROOT, RC0_ROOT, RC1_ROOT, COORD_ROOT})
            fs::remove_all(d);
    }

    i64 coord_exec(const std::string& sql) {
        QueryClient c;
        if (!c.connect(COORD_PORT) || !c.auth(TOKEN))
            return -1;
        return c.exec(sql);
    }

    i64 coord_count(const std::string& sql) {
        QueryClient c;
        if (!c.connect(COORD_PORT) || !c.auth(TOKEN))
            return -1;
        return c.query_row_count(sql);
    }

    i64 node_count(u16 port, const std::string& sql) {
        QueryClient c;
        if (!c.connect(port) || !c.auth(TOKEN))
            return -1;
        return c.query_row_count(sql);
    }

    i64 rc_exec(u16 rc_port, const std::string& sql) {
        QueryClient c;
        if (!c.connect(rc_port) || !c.auth(TOKEN))
            return -1;
        return c.exec(sql);
    }

    static std::unique_ptr<RCProcess> s0l_;
    static std::unique_ptr<RCProcess> s0f_;
    static std::unique_ptr<RCProcess> s1l_;
    static std::unique_ptr<RCProcess> s1f_;
    static std::unique_ptr<RCProcess> rc0_;
    static std::unique_ptr<RCProcess> rc1_;
    static std::unique_ptr<RCProcess> coord_;
};

std::unique_ptr<RCProcess> ReplicaCoordinatorMpTest::s0l_;
std::unique_ptr<RCProcess> ReplicaCoordinatorMpTest::s0f_;
std::unique_ptr<RCProcess> ReplicaCoordinatorMpTest::s1l_;
std::unique_ptr<RCProcess> ReplicaCoordinatorMpTest::s1f_;
std::unique_ptr<RCProcess> ReplicaCoordinatorMpTest::rc0_;
std::unique_ptr<RCProcess> ReplicaCoordinatorMpTest::rc1_;
std::unique_ptr<RCProcess> ReplicaCoordinatorMpTest::coord_;

TEST_F(ReplicaCoordinatorMpTest, ReadFromFollower) {
    ASSERT_GE(coord_exec(partitioned_sql("t1")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t1 VALUES (50, 1)"), 0);

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t1"), 1);
    ASSERT_TRUE(wait_replicated(S0F_PORT, "SELECT * FROM t1", 1));
    EXPECT_TRUE(wait_replicated(RC0_PORT, "SELECT * FROM t1", 1));
}

TEST_F(ReplicaCoordinatorMpTest, WriteViaReadCoordinator) {
    ASSERT_GE(coord_exec(partitioned_sql("t2")), 0);
    ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t2 VALUES (50, 1)"), 0);

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t2"), 1);
    EXPECT_TRUE(wait_replicated(S0F_PORT, "SELECT * FROM t2", 1));
}

TEST_F(ReplicaCoordinatorMpTest, MultiPartitionInsertAndSelectViaCoordinator) {
    ASSERT_GE(coord_exec(partitioned_sql("t_mp")), 0);
    for (int i = 1; i <= 5; ++i)
        ASSERT_GE(coord_exec("INSERT INTO t_mp VALUES (" + std::to_string(i * 10) + ", " +
                             std::to_string(i) + ")"),
                  0);
    for (int i = 1; i <= 5; ++i)
        ASSERT_GE(coord_exec("INSERT INTO t_mp VALUES (" + std::to_string(100 + i) + ", " +
                             std::to_string(i) + ")"),
                  0);

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t_mp"), 5);
    EXPECT_EQ(node_count(S1L_PORT, "SELECT * FROM t_mp"), 5);
    EXPECT_EQ(coord_count("SELECT * FROM t_mp"), 10);
}

TEST_F(ReplicaCoordinatorMpTest, RcRoutesReadsAndWritesIndependently) {
    ASSERT_GE(coord_exec(partitioned_sql("t_rw")), 0);

    ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t_rw VALUES (50, 1)"), 0);
    ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t_rw VALUES (60, 2)"), 0);

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t_rw"), 2);
    ASSERT_TRUE(wait_replicated(S0F_PORT, "SELECT * FROM t_rw", 2));
    for (int i = 0; i < 4; ++i)
        EXPECT_TRUE(wait_replicated(RC0_PORT, "SELECT * FROM t_rw", 2));
}

TEST_F(ReplicaCoordinatorMpTest, FollowerServesReadsAfterReplication) {
    ASSERT_GE(coord_exec(partitioned_sql("t_repl")), 0);

    for (int i = 1; i <= 3; ++i)
        ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t_repl VALUES (" + std::to_string(i * 10) + ", " +
                                        std::to_string(i) + ")"),
                  0);

    ASSERT_TRUE(wait_replicated(S0F_PORT, "SELECT * FROM t_repl", 3));
    EXPECT_TRUE(wait_replicated(RC0_PORT, "SELECT * FROM t_repl", 3));
}

TEST_F(ReplicaCoordinatorMpTest, RcRejectsBadAuthToken) {
    QueryClient c;
    ASSERT_TRUE(c.connect(RC0_PORT));
    EXPECT_FALSE(c.auth("wrong-token"));
}

TEST_F(ReplicaCoordinatorMpTest, BothShardsServeReadsConcurrently) {
    ASSERT_GE(coord_exec(partitioned_sql("t_both")), 0);

    ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t_both VALUES (10, 1)"), 0);
    ASSERT_GE(rc_exec(RC1_PORT, "INSERT INTO t_both VALUES (200, 2)"), 0);

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t_both"), 1);
    EXPECT_EQ(node_count(S1L_PORT, "SELECT * FROM t_both"), 1);

    EXPECT_TRUE(wait_replicated(RC0_PORT, "SELECT * FROM t_both", 1));
    EXPECT_TRUE(wait_replicated(RC1_PORT, "SELECT * FROM t_both", 1));
}

TEST_F(ReplicaCoordinatorMpTest, LeaderFailoverUpdatesReadCoordinator) {
    ASSERT_GE(coord_exec(partitioned_sql("t_fail")), 0);
    ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t_fail VALUES (50, 1)"), 0);
    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t_fail"), 1);

    s0l_->crash();
    bool elected = wait_write_ok(RC0_PORT, "INSERT INTO t_fail VALUES (51, 2)");
    ASSERT_TRUE(elected) << "RC0 did not accept writes after leader failover";

    EXPECT_EQ(node_count(S0F_PORT, "SELECT * FROM t_fail WHERE id < 100"), 2);
}

TEST_F(ReplicaCoordinatorMpTest, ReadContinuesAfterFailover) {
    ASSERT_GE(coord_exec(partitioned_sql("t_read_fail")), 0);
    ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t_read_fail VALUES (50, 1)"), 0);
    EXPECT_TRUE(wait_replicated(RC0_PORT, "SELECT * FROM t_read_fail", 1));
    ASSERT_GE(rc_exec(RC0_PORT, "INSERT INTO t_read_fail VALUES (51, 2)"), 0);
    EXPECT_TRUE(wait_replicated(RC0_PORT, "SELECT * FROM t_read_fail", 2));
}
