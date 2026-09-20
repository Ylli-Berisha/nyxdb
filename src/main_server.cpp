#include "common/logger.h"
#include "server/server.h"

#include <cstdlib>
#include <iostream>
#include <string>

static void usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " --data-dir <path> --token <secret> [--port <n>] [--log-level <level>]\n";
}

int main(int argc, char** argv) {
    std::string data_dir;
    std::string token;
    std::string log_level = "warn";
    uint16_t    port      = 4433;

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
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (data_dir.empty() || token.empty()) {
        usage(argv[0]);
        return 1;
    }

    nyx::init_logger(log_level);

    auto r = nyx::server::Server::create(data_dir, port, token);
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
