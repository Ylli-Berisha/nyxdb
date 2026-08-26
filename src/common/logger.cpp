#include "common/logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace nyx {

void init_logger(std::string_view level) {
    auto sink   = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("nyx", sink);

    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    logger->set_level(spdlog::level::from_str(std::string(level)));

    spdlog::set_default_logger(logger);
}

} // namespace nyx
