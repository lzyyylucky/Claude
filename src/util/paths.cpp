#include "paths.hpp"

#include <cstdlib>

namespace cct::util {

std::filesystem::path default_config_path() {
  return std::filesystem::current_path() / ".cct-cn" / "config.json";
}

std::filesystem::path template_root_from_env_or_default() {
  if (const char* env = std::getenv("CCT_CN_TEMPLATE_DIR")) {
    return std::filesystem::path(env);
  }
#ifdef CCT_CN_TEMPLATE_ROOT
  return std::filesystem::path(CCT_CN_TEMPLATE_ROOT);
#else
  return std::filesystem::current_path() / "templates" / "zh";
#endif
}

}  // namespace cct::util