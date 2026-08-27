#include "common/logger.h"

#include <cstring>
#include <string>

struct Config {
    uint16_t port = 1404;
    std::string data_dir = "./data";
    std::string log_level = "info";
};

static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--port") == 0)
            cfg.port = static_cast<uint16_t>(std::stoi(argv[i + 1]));
        else if (std::strcmp(argv[i], "--data-dir") == 0)
            cfg.data_dir = argv[i + 1];
        else if (std::strcmp(argv[i], "--log-level") == 0)
            cfg.log_level = argv[i + 1];
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);
    nyx::init_logger(cfg.log_level);
    NYX_INFO("nyxdb starting — port={} data_dir={}", cfg.port, cfg.data_dir);
    return 0;
}
