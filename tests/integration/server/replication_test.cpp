#include "query_client.h"
#include "replication/replication_config.h"
#include "server/server.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <thread>

namespace fs = std::filesystem;
using namespace nyx;
using namespace nyx::server;
using namespace nyx::replication;

static constexpr u16 LEADER_PORT = 14440;
static constexpr u16 FOLLOWER_PORT = 14441;
static const std::string TOKEN = "repl-test-secret";
static const std::string LEADER_ROOT = "/tmp/nyxdb_repl_test_leader";
static const std::string FOLLOWER_ROOT = "/tmp/nyxdb_repl_test_follower";

static NodeConfig make_leader_cfg() {
    NodeConfig cfg;
    cfg.node_id = "127.0.0.1";
    cfg.role = NodeConfig::Role::Leader;
    cfg.port = LEADER_PORT;
    cfg.peer_addrs = {"127.0.0.1:14440", "127.0.0.2:14441"};
    cfg.auth_token = TOKEN;
    cfg.election_timeout_min_ms = 500;
    cfg.election_timeout_max_ms = 1000;
    cfg.heartbeat_interval_ms = 200;
    return cfg;
}

static NodeConfig make_follower_cfg() {
    NodeConfig cfg;
    cfg.node_id = "127.0.0.2";
    cfg.role = NodeConfig::Role::Follower;
    cfg.port = FOLLOWER_PORT;
    cfg.peer_addrs = {"127.0.0.1:14440", "127.0.0.2:14441"};
    cfg.auth_token = TOKEN;
    cfg.leader_addr = "127.0.0.1:14440";
    cfg.election_timeout_min_ms = 500;
    cfg.election_timeout_max_ms = 1000;
    cfg.heartbeat_interval_ms = 200;
    return cfg;
}

class ReplicationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fs::remove_all(LEADER_ROOT);
        fs::remove_all(FOLLOWER_ROOT);

        {
            auto r = Server::create(LEADER_ROOT, LEADER_PORT, TOKEN, make_leader_cfg());
            ASSERT_TRUE(r.is_ok()) << r.error().message;
            leader_ = std::make_unique<Server>(std::move(r.value()));
            ASSERT_TRUE(leader_->start().is_ok());
        }
        {
            auto r = Server::create(FOLLOWER_ROOT, FOLLOWER_PORT, TOKEN, make_follower_cfg());
            ASSERT_TRUE(r.is_ok()) << r.error().message;
            follower_ = std::make_unique<Server>(std::move(r.value()));
            ASSERT_TRUE(follower_->start().is_ok());
        }
    }

    void TearDown() override {
        follower_.reset();
        leader_.reset();
        fs::remove_all(LEADER_ROOT);
        fs::remove_all(FOLLOWER_ROOT);
    }

    bool wait_for(std::function<bool()> fn, std::chrono::seconds timeout = std::chrono::seconds(8),
                  std::chrono::milliseconds poll = std::chrono::milliseconds(50)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (fn())
                return true;
            std::this_thread::sleep_for(poll);
        }
        return false;
    }

    bool wait_replicated(u16 port, const std::string& sql, i64 expected,
                         std::chrono::seconds timeout = std::chrono::seconds(8)) {
        QueryClient c;
        if (!c.connect(port) || !c.auth(TOKEN))
            return false;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            i64 n = c.query_row_count(sql);
            if (n == expected)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    i64 select_count(u16 port, const std::string& sql) {
        QueryClient c;
        if (!c.connect(port))
            return -2;
        if (!c.auth(TOKEN))
            return -2;
        return c.query_row_count(sql);
    }

    std::unique_ptr<Server> leader_;
    std::unique_ptr<Server> follower_;
};

TEST_F(ReplicationTest, FollowerReplicatesFromLeader) {
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

TEST_F(ReplicationTest, WriteForwardingFromFollower) {
    {
        QueryClient fc;
        ASSERT_TRUE(fc.connect(FOLLOWER_PORT));
        ASSERT_TRUE(fc.auth(TOKEN));
        ASSERT_GE(fc.exec("CREATE TABLE fwd_t (x INT NOT NULL)"), 0);
        ASSERT_GE(fc.exec("INSERT INTO fwd_t VALUES (10), (20)"), 0);
    }

    bool on_follower = wait_replicated(FOLLOWER_PORT, "SELECT x FROM fwd_t", 2);
    EXPECT_TRUE(on_follower) << "forwarded writes not visible on follower within timeout";

    EXPECT_EQ(select_count(LEADER_PORT, "SELECT x FROM fwd_t"), 2);
}

TEST_F(ReplicationTest, ElectionOnLeaderFailure) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE elect_t (v INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO elect_t VALUES (1)"), 0);
    }

    bool synced = wait_replicated(FOLLOWER_PORT, "SELECT v FROM elect_t", 1);
    ASSERT_TRUE(synced) << "follower did not sync initial data before leader failure";

    leader_.reset();

    bool elected = wait_for([this] { return follower_->is_leader(); }, std::chrono::seconds(3),
                            std::chrono::milliseconds(50));
    ASSERT_TRUE(elected) << "follower did not become leader within 3s";

    QueryClient fc;
    ASSERT_TRUE(fc.connect(FOLLOWER_PORT));
    ASSERT_TRUE(fc.auth(TOKEN));
    ASSERT_EQ(fc.exec("INSERT INTO elect_t VALUES (2)"), 1);
    EXPECT_EQ(fc.query_row_count("SELECT v FROM elect_t"), 2);
}

TEST_F(ReplicationTest, OldLeaderRejoinsAsFollower) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE rejoin_t (v INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO rejoin_t VALUES (1)"), 0);
    }

    bool synced = wait_replicated(FOLLOWER_PORT, "SELECT v FROM rejoin_t", 1);
    ASSERT_TRUE(synced) << "follower did not sync initial data";

    leader_.reset();

    bool elected = wait_for([this] { return follower_->is_leader(); }, std::chrono::seconds(3),
                            std::chrono::milliseconds(50));
    ASSERT_TRUE(elected) << "follower did not become leader within 3s";

    {
        QueryClient fc;
        ASSERT_TRUE(fc.connect(FOLLOWER_PORT));
        ASSERT_TRUE(fc.auth(TOKEN));
        ASSERT_EQ(fc.exec("INSERT INTO rejoin_t VALUES (2)"), 1);
    }

    {
        auto r = Server::create(LEADER_ROOT, LEADER_PORT, TOKEN, make_leader_cfg());
        ASSERT_TRUE(r.is_ok()) << r.error().message;
        leader_ = std::make_unique<Server>(std::move(r.value()));
        ASSERT_TRUE(leader_->start().is_ok());
    }

    bool stepped_down = wait_for([this] { return !leader_->is_leader(); }, std::chrono::seconds(5),
                                 std::chrono::milliseconds(50));
    ASSERT_TRUE(stepped_down) << "old leader did not step down within 5s";

    bool resynced =
        wait_replicated(LEADER_PORT, "SELECT v FROM rejoin_t", 2, std::chrono::seconds(10));
    EXPECT_TRUE(resynced) << "old leader did not resync the missing row within 10s";
}

TEST_F(ReplicationTest, FollowerCrashRecovery) {
    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("CREATE TABLE crash_t (v INT NOT NULL)"), 0);
        ASSERT_GE(lc.exec("INSERT INTO crash_t VALUES (1), (2), (3)"), 0);
    }

    bool synced = wait_replicated(FOLLOWER_PORT, "SELECT v FROM crash_t", 3);
    ASSERT_TRUE(synced) << "follower did not sync initial data before crash";

    follower_.reset();
    fs::remove(FOLLOWER_ROOT + "/replication_state.bin");

    {
        QueryClient lc;
        ASSERT_TRUE(lc.connect(LEADER_PORT));
        ASSERT_TRUE(lc.auth(TOKEN));
        ASSERT_GE(lc.exec("INSERT INTO crash_t VALUES (4), (5)"), 0);
    }

    {
        auto r = Server::create(FOLLOWER_ROOT, FOLLOWER_PORT, TOKEN, make_follower_cfg());
        ASSERT_TRUE(r.is_ok()) << r.error().message;
        follower_ = std::make_unique<Server>(std::move(r.value()));
        ASSERT_TRUE(follower_->start().is_ok());
    }

    bool recovered =
        wait_replicated(FOLLOWER_PORT, "SELECT v FROM crash_t", 5, std::chrono::seconds(15));
    EXPECT_TRUE(recovered) << "follower did not recover all 5 rows after crash + snapshot resync";
}
