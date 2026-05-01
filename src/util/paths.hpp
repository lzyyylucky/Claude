#pragma once

#include <filesystem>
#include <string>

namespace cct::util {

std::filesystem::path default_config_path();
std::filesystem::path template_root_from_env_or_default();

}
