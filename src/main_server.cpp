#include "common/logger.h"
#include "replication/replication_config.h"
#include "server/server.h"

#include <cstdlib>
#include <iostream>
#include <string>

static void usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " --data-dir <path> --token <secret>"
                 " [--port <n>] [--log-level <level>]"
                 " [--node-id <id> --role leader|follower"
                 " --peers <addr:port,...>"
                 " [--leader-addr <host:port>] [--max-wal-lag-mb <n>]]\n";
}

int main(int argc, char** argv) {
    std::string data_dir;
    std::string token;
    std::string log_level = "warn";
    uint16_t port = 4433;
    std::string node_id;
    std::string role_str;
    std::string peers_str;
    std::string leader_addr;
    uint64_t max_wal_lag_mb = 4096;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--data-dir" || a == "-d") && i + 1 < argc)
            data_dir = argv[++i];
        else if ((a == "--token" || a == "-t") && i + 1 < argc)
            token = argv[++i];
        else if ((a == "--port" || a == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if ((a == "--log-level" || a == "-l") && i + 1 < argc)
            log_level = argv[++i];
        else if (a == "--node-id" && i + 1 < argc)
            node_id = argv[++i];
        else if (a == "--role" && i + 1 < argc)
            role_str = argv[++i];
        else if (a == "--peers" && i + 1 < argc)
            peers_str = argv[++i];
        else if (a == "--leader-addr" && i + 1 < argc)
            leader_addr = argv[++i];
        else if (a == "--max-wal-lag-mb" && i + 1 < argc)
            max_wal_lag_mb = std::stoull(argv[++i]);
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (data_dir.empty() || token.empty()) {
        usage(argv[0]);
        return 1;
    }

    nyx::replication::NodeConfig node_cfg;
    node_cfg.port = port;
    node_cfg.max_wal_lag_bytes = max_wal_lag_mb * 1024ULL * 1024ULL;
    node_cfg.auth_token = token;
    node_cfg.leader_addr = leader_addr;

    if (!role_str.empty()) {
        if (node_id.empty() || peers_str.empty()) {
            std::cerr << "error: --node-id and --peers are required when --role is set\n";
            return 1;
        }
        node_cfg.node_id = node_id;
        if (role_str == "leader")
            node_cfg.role = nyx::replication::NodeConfig::Role::Leader;
        else if (role_str == "follower")
            node_cfg.role = nyx::replication::NodeConfig::Role::Follower;
        else {
            std::cerr << "error: --role must be leader or follower\n";
            return 1;
        }
        std::string peer;
        for (char ch : peers_str) {
            if (ch == ',') {
                if (!peer.empty())
                    node_cfg.peer_addrs.push_back(peer);
                peer.clear();
            } else {
                peer += ch;
            }
        }
        if (!peer.empty())
            node_cfg.peer_addrs.push_back(peer);
    }

    nyx::init_logger(log_level);

    auto r = nyx::server::Server::create(data_dir, port, token, node_cfg);
    if (!r.is_ok()) {
        std::cerr << "error: " << r.error().message << "\n";
        return 1;
    }

    std::cout << "nyxdb server listening on :" << port << "\n";

    auto run_r = r.value().run();
    if (!run_r.is_ok()) {
        std::cerr << "error: " << run_r.error().message << "\n";
        return 1;
    }

    return 0;
}
