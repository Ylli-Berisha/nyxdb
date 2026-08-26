#pragma once

#include <spdlog/spdlog.h>
#include <string_view>

namespace nyx {

void init_logger(std::string_view level = "info");

} // namespace nyx

#define NYX_INFO(...)  spdlog::info(__VA_ARGS__)
#define NYX_WARN(...)  spdlog::warn(__VA_ARGS__)
#define NYX_ERROR(...) spdlog::error(__VA_ARGS__)
#define NYX_DEBUG(...) spdlog::debug(__VA_ARGS__)
