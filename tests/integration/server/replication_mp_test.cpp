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

static constexpr u16 LEADER_PORT = 15450;
static constexpr u16 FOLLOWER_PORT = 15451;
static const std::string TOKEN = "mp-repl-test";
static const std::string LEADER_ROOT = "/tmp/nyxdb_mp_leader";
static const std::string FOLLOWER_ROOT = "/tmp/nyxdb_mp_follower";
static const std::string PEERS = "127.0.0.1:15450,127.0.0.2:15451";

struct ServerProcess {
    pid_t pid = -1;
    u16 port = 0;
    std::string data_dir;

    ServerProcess() = default;
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;
    ServerProcess(ServerProcess&& o) noexcept
        : pid(o.pid), port(o.port), data_dir(std::move(o.data_dir)) {
        o.pid = -1;
    }
    ~ServerProcess() { stop(); }

    static ServerProcess spawn_leader(const std::string& data_dir, u16 port,
                                      const std::string& token) {
        return spawn_(data_dir, port, token, "127.0.0.1", "leader", PEERS, "");
    }

    static ServerProcess spawn_follower(const std::string& data_dir, u16 port,
                                        const std::string& token, const std::string& leader_addr) {
        return spawn_(data_dir, port, token, "127.0.0.2", "follower", PEERS, leader_addr);
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

  private:
    static ServerProcess spawn_(const std::string& data_dir, u16 port, const std::string& token,
                                const std::string& node_id, const std::string& role,
                                const std::string& peers, const std::string& leader_addr) {
        ServerProcess sp;
        sp.port = port;
        sp.data_dir = data_dir;
        fs::create_directories(data_dir);

        pid_t pid = ::fork();
        if (pid == 0) {
            std::vector<std::string> args = {
                NYXDB_SERVER_BIN,
                "--data-dir",
                data_dir,
                "--token",
                token,
                "--port",
                std::to_string(port),
                "--log-level",
                "error",
                "--node-id",
                node_id,
                "--role",
                role,
                "--peers",
                peers,
            };
            if (!leader_addr.empty()) {
                args.push_back("--leader-addr");
                args.push_back(leader_addr);
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
        sp.pid = pid;
        return sp;
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

class ReplicationMpTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(LEADER_ROOT);
        fs::remove_all(FOLLOWER_ROOT);

        leader_ = std::make_unique<ServerProcess>(
            ServerProcess::spawn_leader(LEADER_ROOT, LEADER_PORT, TOKEN));
        ASSERT_GT(leader_->pid, 0);
        ASSERT_TRUE(wait_ready(LEADER_PORT, TOKEN)) << "leader did not start";

        follower_ = std::make_unique<ServerProcess>(ServerProcess::spawn_follower(
            FOLLOWER_ROOT, FOLLOWER_PORT, TOKEN, "127.0.0.1:" + std::to_string(LEADER_PORT)));
        ASSERT_GT(follower_->pid, 0);
        ASSERT_TRUE(wait_ready(FOLLOWER_PORT, TOKEN)) << "follower did not start";
    }

    void TearDown() override {
        follower_.reset();
        leader_.reset();
        fs::remove_all(LEADER_ROOT);
        fs::remove_all(FOLLOWER_ROOT);
    }

    bool wait_replicated(u16 port, const std::string& sql, i64 expected,
                         std::chrono::seconds timeout = std::chrono::seconds(10)) {
        QueryClient c;
        if (!c.connect(port) || !c.auth(TOKEN))
            return false;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (c.query_row_count(sql) == expected)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    i64 select_count(u16 port, const std::string& sql) {
        QueryClient c;
        if (!c.connect(port) || !c.auth(TOKEN))
            return -2;
        return c.query_row_count(sql);
    }

    // Polls until a write to `port` succeeds — used to detect when a node wins election.
    bool wait_write_ok(u16 port, const std::string& sql,
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

    std::unique_ptr<ServerProcess> leader_;
    std::unique_ptr<ServerProcess> follower_;
};

TEST_F(ReplicationMpTest, FollowerReplicatesFromLeader) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE rep_t (id INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO rep_t VALUES (1), (2), (3)"), 0);
    }
    bool ok = wait_replicated(FOLLOWER_PORT, "SELECT id FROM rep_t", 3);
    EXPECT_TRUE(ok) << "follower did not replicate 3 rows within timeout";
}

TEST_F(ReplicationMpTest, WriteForwardingFromFollower) {
    {
        QueryClient fc;
        ASSERT_TRUE(fc.connect(FOLLOWER_PORT));
        ASSERT_TRUE(fc.auth(TOKEN));
        ASSERT_GE(fc.exec("CREATE TABLE fwd_t (x INT NOT NULL)"), 0);
        ASSERT_GE(fc.exec("INSERT INTO fwd_t VALUES (10), (20)"), 0);
    }
    bool on_follower = wait_replicated(FOLLOWER_PORT, "SELECT x FROM fwd_t", 2);
    EXPECT_TRUE(on_follower) << "forwarded writes not visible on follower";
    EXPECT_EQ(select_count(LEADER_PORT, "SELECT x FROM fwd_t"), 2);
}

TEST_F(ReplicationMpTest, ElectionOnLeaderFailure) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE elect_t (v INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO elect_t VALUES (1)"), 0);
    }
    bool synced = wait_replicated(FOLLOWER_PORT, "SELECT v FROM elect_t", 1);
    ASSERT_TRUE(synced) << "follower did not sync initial row before leader crash";

    leader_->crash();

    // follower wins election after 3-5 s timeout; poll until it accepts writes
    bool elected = wait_write_ok(FOLLOWER_PORT, "INSERT INTO elect_t VALUES (2)");
    ASSERT_TRUE(elected) << "follower did not become leader within timeout";

    EXPECT_EQ(select_count(FOLLOWER_PORT, "SELECT v FROM elect_t"), 2);
}

TEST_F(ReplicationMpTest, OldLeaderRejoinsAsFollower) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE rejoin_t (v INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO rejoin_t VALUES (1)"), 0);
    }
    bool synced = wait_replicated(FOLLOWER_PORT, "SELECT v FROM rejoin_t", 1);
    ASSERT_TRUE(synced) << "follower did not sync before leader crash";

    leader_->crash();

    bool elected = wait_write_ok(FOLLOWER_PORT, "INSERT INTO rejoin_t VALUES (2)");
    ASSERT_TRUE(elected) << "follower did not win election";

    // restart old leader — it has a stale term so it will step down and resync
    leader_ = std::make_unique<ServerProcess>(
        ServerProcess::spawn_leader(LEADER_ROOT, LEADER_PORT, TOKEN));
    ASSERT_TRUE(wait_ready(LEADER_PORT, TOKEN)) << "old leader did not restart";

    bool resynced =
        wait_replicated(LEADER_PORT, "SELECT v FROM rejoin_t", 2, std::chrono::seconds(15));
    EXPECT_TRUE(resynced) << "old leader did not resync the row written while it was down";
}

TEST_F(ReplicationMpTest, FollowerCrashRecovery) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE crash_t (v INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO crash_t VALUES (1), (2), (3)"), 0);
    }
    bool synced = wait_replicated(FOLLOWER_PORT, "SELECT v FROM crash_t", 3);
    ASSERT_TRUE(synced) << "follower did not sync initial rows before crash";

    follower_->crash();
    fs::remove(FOLLOWER_ROOT + "/replication_state.bin");

    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("INSERT INTO crash_t VALUES (4), (5)"), 0);
    }

    follower_ = std::make_unique<ServerProcess>(ServerProcess::spawn_follower(
        FOLLOWER_ROOT, FOLLOWER_PORT, TOKEN, "127.0.0.1:" + std::to_string(LEADER_PORT)));
    ASSERT_TRUE(wait_ready(FOLLOWER_PORT, TOKEN)) << "follower did not restart";

    bool recovered =
        wait_replicated(FOLLOWER_PORT, "SELECT v FROM crash_t", 5, std::chrono::seconds(15));
    EXPECT_TRUE(recovered) << "follower did not recover all 5 rows via snapshot resync";
}
