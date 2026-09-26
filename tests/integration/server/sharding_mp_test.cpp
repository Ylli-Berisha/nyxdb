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

static constexpr u16 SHARD0_PORT = 15460;
static constexpr u16 SHARD1_PORT = 15461;
static constexpr u16 COORD_PORT = 15462;
static const std::string TOKEN = "shard-test-token";
static const std::string SHARD0_ROOT = "/tmp/nyxdb_shard0";
static const std::string SHARD1_ROOT = "/tmp/nyxdb_shard1";
static const std::string COORD_ROOT = "/tmp/nyxdb_coord";
static const std::string SHARD0_ADDR = "127.0.0.1:15460";
static const std::string SHARD1_ADDR = "127.0.0.1:15461";

struct ShardProcess {
    pid_t pid = -1;
    u16 port = 0;
    std::string data_dir;

    ShardProcess() = default;
    ShardProcess(const ShardProcess&) = delete;
    ShardProcess& operator=(const ShardProcess&) = delete;
    ShardProcess(ShardProcess&& o) noexcept
        : pid(o.pid), port(o.port), data_dir(std::move(o.data_dir)) {
        o.pid = -1;
    }
    ~ShardProcess() { stop(); }

    static ShardProcess spawn(const std::string& data_dir, u16 port, const std::string& token,
                              const std::string& role = "") {
        ShardProcess sp;
        sp.port = port;
        sp.data_dir = data_dir;
        fs::create_directories(data_dir);

        pid_t child = ::fork();
        if (child == 0) {
            std::vector<std::string> args = {
                NYXDB_SERVER_BIN, "--data-dir",         data_dir,      "--token", token,
                "--port",         std::to_string(port), "--log-level", "error",
            };
            if (!role.empty()) {
                args.push_back("--role");
                args.push_back(role);
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

static std::string partitioned_table_sql(const std::string& name) {
    return "CREATE TABLE " + name +
           " (id INT NOT NULL, val INT NOT NULL) "
           "PARTITION BY RANGE (id) ("
           "PARTITION p0 VALUES LESS THAN (100) ON '" +
           SHARD0_ADDR +
           "', "
           "PARTITION p1 VALUES LESS THAN (MAXVALUE) ON '" +
           SHARD1_ADDR + "')";
}

class ShardingMpTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        fs::remove_all(SHARD0_ROOT);
        fs::remove_all(SHARD1_ROOT);
        fs::remove_all(COORD_ROOT);

        shard0_ =
            std::make_unique<ShardProcess>(ShardProcess::spawn(SHARD0_ROOT, SHARD0_PORT, TOKEN));
        ASSERT_GT(shard0_->pid, 0);
        ASSERT_TRUE(wait_ready(SHARD0_PORT, TOKEN)) << "shard0 did not start";

        shard1_ =
            std::make_unique<ShardProcess>(ShardProcess::spawn(SHARD1_ROOT, SHARD1_PORT, TOKEN));
        ASSERT_GT(shard1_->pid, 0);
        ASSERT_TRUE(wait_ready(SHARD1_PORT, TOKEN)) << "shard1 did not start";

        coord_ = std::make_unique<ShardProcess>(
            ShardProcess::spawn(COORD_ROOT, COORD_PORT, TOKEN, "coordinator"));
        ASSERT_GT(coord_->pid, 0);
        ASSERT_TRUE(wait_ready(COORD_PORT, TOKEN)) << "coordinator did not start";
    }

    static void TearDownTestSuite() {
        coord_.reset();
        shard1_.reset();
        shard0_.reset();
        fs::remove_all(SHARD0_ROOT);
        fs::remove_all(SHARD1_ROOT);
        fs::remove_all(COORD_ROOT);
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

    i64 shard_count(u16 port, const std::string& sql) {
        QueryClient c;
        if (!c.connect(port) || !c.auth(TOKEN))
            return -1;
        return c.query_row_count(sql);
    }

    static std::unique_ptr<ShardProcess> shard0_;
    static std::unique_ptr<ShardProcess> shard1_;
    static std::unique_ptr<ShardProcess> coord_;
};

std::unique_ptr<ShardProcess> ShardingMpTest::shard0_;
std::unique_ptr<ShardProcess> ShardingMpTest::shard1_;
std::unique_ptr<ShardProcess> ShardingMpTest::coord_;

TEST_F(ShardingMpTest, InsertRoutesToFirstPartition) {
    ASSERT_GE(coord_exec(partitioned_table_sql("t1")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t1 VALUES (50, 1)"), 0);

    EXPECT_EQ(shard_count(SHARD0_PORT, "SELECT * FROM t1"), 1);
    EXPECT_EQ(coord_count("SELECT * FROM t1"), 1);
}

TEST_F(ShardingMpTest, InsertRoutesToMaxvaluePartition) {
    ASSERT_GE(coord_exec(partitioned_table_sql("t2")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t2 VALUES (200, 1)"), 0);

    EXPECT_EQ(shard_count(SHARD1_PORT, "SELECT * FROM t2"), 1);
    EXPECT_EQ(coord_count("SELECT * FROM t2"), 1);
}

TEST_F(ShardingMpTest, SelectFanOutReturnsAllRows) {
    ASSERT_GE(coord_exec(partitioned_table_sql("t3")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t3 VALUES (10, 1)"), 0);
    ASSERT_GE(coord_exec("INSERT INTO t3 VALUES (200, 2)"), 0);

    EXPECT_EQ(coord_count("SELECT * FROM t3"), 2);
}

TEST_F(ShardingMpTest, SelectWherePartitionColPruning) {
    ASSERT_GE(coord_exec(partitioned_table_sql("t4")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t4 VALUES (10, 1)"), 0);
    ASSERT_GE(coord_exec("INSERT INTO t4 VALUES (200, 2)"), 0);

    EXPECT_EQ(coord_count("SELECT * FROM t4 WHERE id < 100"), 1);
}

TEST_F(ShardingMpTest, MultiRowInsertSplitAcrossPartitions) {
    ASSERT_GE(coord_exec(partitioned_table_sql("t5")), 0);
    ASSERT_GE(coord_exec("INSERT INTO t5 VALUES (10,1),(20,2),(110,3),(120,4)"), 0);

    EXPECT_EQ(shard_count(SHARD0_PORT, "SELECT * FROM t5"), 2);
    EXPECT_EQ(shard_count(SHARD1_PORT, "SELECT * FROM t5"), 2);
    EXPECT_EQ(coord_count("SELECT * FROM t5"), 4);
}

TEST_F(ShardingMpTest, AlterAddPartitionMigratesRows) {
    std::string create_sql = "CREATE TABLE t6 (id INT NOT NULL, val INT NOT NULL) "
                             "PARTITION BY RANGE (id) ("
                             "PARTITION p0 VALUES LESS THAN (MAXVALUE) ON '" +
                             SHARD0_ADDR + "')";
    ASSERT_GE(coord_exec(create_sql), 0);
    ASSERT_GE(coord_exec("INSERT INTO t6 VALUES (5,1),(15,2),(105,3),(115,4)"), 0);

    EXPECT_EQ(shard_count(SHARD0_PORT, "SELECT * FROM t6"), 4);

    std::string alter_sql =
        "ALTER TABLE t6 ADD PARTITION p1 VALUES LESS THAN (100) ON '" + SHARD1_ADDR + "'";
    ASSERT_GE(coord_exec(alter_sql), 0);

    EXPECT_EQ(shard_count(SHARD0_PORT, "SELECT * FROM t6"), 2);
    EXPECT_EQ(shard_count(SHARD1_PORT, "SELECT * FROM t6"), 2);
}
