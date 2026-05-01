#pragma once

#include <filesystem>
#include <string>

namespace cct::util {

struct AppConfig {
  /** anthropic：Claude Messages API；zhipu：智谱 chat completions（OpenAI 兼容） */
  std::string llm_provider = "anthropic";
  std::string api_key;
  std::string model = "claude-sonnet-4-20250514";
  std::string api_host = "api.anthropic.com";
  std::string api_path = "/v1/messages";
  int max_context_chars = 12000;
  int max_tokens = 4096;
  bool use_mock = false;
  /** 仅免费档：单日模型调用上限（>0 生效）；0 表示免费档不限。付费档使用各订阅档位内置上限。 */
  int llm_daily_call_limit = 0;
  /** Web IDE 服务端工作区根目录（UTF-8 路径，空则禁用 /api/workspace/*） */
  std::string workspace_root;
  /** Git 附属 Node 服务监听端口（127.0.0.1）；与 git-worker/index.js 一致 */
  int git_worker_port = 47821;
  /** serve 启动时尝试自动拉起 git-worker（需已 npm install 且 PATH 有 node） */
  bool git_worker_autostart = true;
  /** SQL Server ODBC 连接串（UTF-8）；Web 服务必须配置后才能启动 */
  std::string sql_odbc_connection_string;
};

bool load_config(const std::filesystem::path& path, AppConfig& out, std::string& error);
void save_example_config(const std::filesystem::path& path, std::string& error);

/** serve 下 --workspace 覆盖 workspace_root（UTF-8）；实现在 config.cpp，避免部分 IntelliSense 对内联声明误报 E0135 */
void apply_cli_workspace_override(AppConfig& cfg, const char* utf8_path_if_nonnull);

}
