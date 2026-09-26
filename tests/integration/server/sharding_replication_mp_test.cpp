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

static constexpr u16 S0L_PORT = 15470;
static constexpr u16 S0F_PORT = 15471;
static constexpr u16 S1L_PORT = 15472;
static constexpr u16 S1F_PORT = 15473;
static constexpr u16 COORD_PORT = 15474;

static const std::string TOKEN = "sr-test-token";

static const std::string S0L_ROOT = "/tmp/nyxdb_sr_s0l";
static const std::string S0F_ROOT = "/tmp/nyxdb_sr_s0f";
static const std::string S1L_ROOT = "/tmp/nyxdb_sr_s1l";
static const std::string S1F_ROOT = "/tmp/nyxdb_sr_s1f";
static const std::string COORD_ROOT = "/tmp/nyxdb_sr_coord";

static const std::string S0_PEERS = "127.0.0.1:15470,127.0.0.2:15471";
static const std::string S1_PEERS = "127.0.0.3:15472,127.0.0.4:15473";
static const std::string COORD_ADDR = "127.0.0.1:15474";

static const std::string S0L_ADDR = "127.0.0.1:15470";
static const std::string S1L_ADDR = "127.0.0.3:15472";

struct SRProcess {
    pid_t pid = -1;
    u16 port = 0;
    std::string data_dir;

    SRProcess() = default;
    SRProcess(const SRProcess&) = delete;
    SRProcess& operator=(const SRProcess&) = delete;
    SRProcess(SRProcess&& o) noexcept : pid(o.pid), port(o.port), data_dir(std::move(o.data_dir)) {
        o.pid = -1;
    }
    ~SRProcess() { stop(); }

    static SRProcess spawn(const std::string& data_dir, u16 port, const std::string& token,
                           const std::string& role, const std::string& node_id = "",
                           const std::string& peers = "", const std::string& leader_addr = "",
                           const std::string& coordinator_addr = "") {
        SRProcess sp;
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
                          std::chrono::seconds timeout = std::chrono::seconds(12)) {
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
           S0L_ADDR +
           "',"
           "  PARTITION p1 VALUES LESS THAN (MAXVALUE) ON '" +
           S1L_ADDR + "')";
}

class ShardingReplicationMpTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        for (const auto& d : {S0L_ROOT, S0F_ROOT, S1L_ROOT, S1F_ROOT, COORD_ROOT})
            fs::remove_all(d);

        s0l_ = std::make_unique<SRProcess>(SRProcess::spawn(S0L_ROOT, S0L_PORT, TOKEN, "leader",
                                                            "127.0.0.1", S0_PEERS, "", COORD_ADDR));
        ASSERT_GT(s0l_->pid, 0);
        ASSERT_TRUE(wait_ready(S0L_PORT, TOKEN)) << "shard0-leader did not start";

        s0f_ = std::make_unique<SRProcess>(
            SRProcess::spawn(S0F_ROOT, S0F_PORT, TOKEN, "follower", "127.0.0.2", S0_PEERS,
                             "127.0.0.1:" + std::to_string(S0L_PORT), COORD_ADDR));
        ASSERT_GT(s0f_->pid, 0);
        ASSERT_TRUE(wait_ready(S0F_PORT, TOKEN)) << "shard0-follower did not start";

        s1l_ = std::make_unique<SRProcess>(SRProcess::spawn(S1L_ROOT, S1L_PORT, TOKEN, "leader",
                                                            "127.0.0.3", S1_PEERS, "", COORD_ADDR));
        ASSERT_GT(s1l_->pid, 0);
        ASSERT_TRUE(wait_ready(S1L_PORT, TOKEN)) << "shard1-leader did not start";

        s1f_ = std::make_unique<SRProcess>(
            SRProcess::spawn(S1F_ROOT, S1F_PORT, TOKEN, "follower", "127.0.0.4", S1_PEERS,
                             "127.0.0.3:" + std::to_string(S1L_PORT), COORD_ADDR));
        ASSERT_GT(s1f_->pid, 0);
        ASSERT_TRUE(wait_ready(S1F_PORT, TOKEN)) << "shard1-follower did not start";

        coord_ = std::make_unique<SRProcess>(
            SRProcess::spawn(COORD_ROOT, COORD_PORT, TOKEN, "coordinator"));
        ASSERT_GT(coord_->pid, 0);
        ASSERT_TRUE(wait_ready(COORD_PORT, TOKEN)) << "coordinator did not start";
    }

    static void TearDownTestSuite() {
        coord_.reset();
        s1f_.reset();
        s1l_.reset();
        s0f_.reset();
        s0l_.reset();
        for (const auto& d : {S0L_ROOT, S0F_ROOT, S1L_ROOT, S1F_ROOT, COORD_ROOT})
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

    static std::unique_ptr<SRProcess> s0l_;
    static std::unique_ptr<SRProcess> s0f_;
    static std::unique_ptr<SRProcess> s1l_;
    static std::unique_ptr<SRProcess> s1f_;
    static std::unique_ptr<SRProcess> coord_;
};

std::unique_ptr<SRProcess> ShardingReplicationMpTest::s0l_;
std::unique_ptr<SRProcess> ShardingReplicationMpTest::s0f_;
std::unique_ptr<SRProcess> ShardingReplicationMpTest::s1l_;
std::unique_ptr<SRProcess> ShardingReplicationMpTest::s1f_;
std::unique_ptr<SRProcess> ShardingReplicationMpTest::coord_;

TEST_F(ShardingReplicationMpTest, InsertRoutesToLeader) {
    ASSERT_GE(coord_exec(partitioned_sql("t1")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t1 VALUES (50, 1)"), 0);

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t1"), 1);
    EXPECT_TRUE(wait_replicated(S0F_PORT, "SELECT * FROM t1", 1));
}

TEST_F(ShardingReplicationMpTest, ReplicationPropagatesAcrossShards) {
    ASSERT_GE(coord_exec(partitioned_sql("t2")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t2 VALUES (50, 1)"), 0);
    ASSERT_GE(coord_exec("INSERT INTO t2 VALUES (200, 2)"), 0);

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t2"), 1);
    EXPECT_EQ(node_count(S1L_PORT, "SELECT * FROM t2"), 1);
    EXPECT_TRUE(wait_replicated(S0F_PORT, "SELECT * FROM t2", 1));
    EXPECT_TRUE(wait_replicated(S1F_PORT, "SELECT * FROM t2", 1));
}

TEST_F(ShardingReplicationMpTest, FollowerForwardsWriteToLeader) {
    ASSERT_GE(coord_exec(partitioned_sql("t3")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t3 VALUES (50, 1)"), 0);

    {
        QueryClient c;
        ASSERT_TRUE(c.connect(S0F_PORT) && c.auth(TOKEN));
        ASSERT_GE(c.exec("INSERT INTO t3 VALUES (60, 2)"), 0);
    }

    EXPECT_EQ(node_count(S0L_PORT, "SELECT * FROM t3"), 2);
    EXPECT_TRUE(wait_replicated(S0F_PORT, "SELECT * FROM t3", 2));
}

TEST_F(ShardingReplicationMpTest, LeaderFailoverNotifiesCoordinator) {
    ASSERT_GE(coord_exec(partitioned_sql("t_fail")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t_fail VALUES (50, 1)"), 0);
    ASSERT_EQ(node_count(S0L_PORT, "SELECT * FROM t_fail"), 1);

    s0l_->crash();

    bool elected = wait_write_ok(S0F_PORT, "INSERT INTO t_fail VALUES (51, 2)");
    ASSERT_TRUE(elected) << "shard0-follower did not win election";

    // coordinator should have received NOTIFY_LEADER and updated its shard map
    // new inserts for p0 (id < 100) must route to the promoted follower at S0F_PORT
    bool routed = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        QueryClient c;
        if (c.connect(COORD_PORT) && c.auth(TOKEN) &&
            c.exec("INSERT INTO t_fail VALUES (55, 3)") >= 0) {
            routed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ASSERT_TRUE(routed) << "coordinator did not re-route after leader failover";

    EXPECT_EQ(node_count(S0F_PORT, "SELECT * FROM t_fail WHERE id < 100"), 3);
}
