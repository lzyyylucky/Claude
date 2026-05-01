#pragma once

#include <cstdint>
#include <filesystem>

namespace cct::util {
struct AppConfig;
}

namespace cct::web {

void run_http_server(std::uint16_t port, const std::filesystem::path& ui_root,
                     const std::filesystem::path& data_dir, const cct::util::AppConfig& llm_cfg);

}
