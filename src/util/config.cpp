#include "config.hpp"

#include "json_minimal.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <regex>

namespace cct::util {

static bool re_int(const std::string& j, const char* key, int& out) {
  std::string pat = std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+)";
  std::regex re(pat);
  std::smatch m;
  if (!std::regex_search(j, m, re)) return false;
  out = std::stoi(m[1].str());
  return true;
}

static bool re_bool(const std::string& j, const char* key, bool& out) {
  std::string pat = std::string("\"") + key + "\"\\s*:\\s*(true|false)";
  std::regex re(pat);
  std::smatch m;
  if (!std::regex_search(j, m, re)) return false;
  out = (m[1].str() == "true");
  return true;
}

bool load_config(const std::filesystem::path& path, AppConfig& out, std::string& error) {
  std::ifstream f(path);
  if (!f) {
    error = "无法打开配置文件: " + path.string();
    return false;
  }
  std::stringstream buf;
  buf << f.rdbuf();
  std::string j = buf.str();
  try {
    std::string s;
    if (json_extract_string_after_key(j, "llm_provider", s)) out.llm_provider = s;
    if (json_extract_string_after_key(j, "api_key", s)) out.api_key = s;
    if (json_extract_string_after_key(j, "model", s)) out.model = s;
    if (json_extract_string_after_key(j, "api_host", s)) out.api_host = s;
    if (json_extract_string_after_key(j, "api_path", s)) out.api_path = s;
    int n = 0;
    if (re_int(j, "max_context_chars", n)) out.max_context_chars = n;
    if (re_int(j, "max_tokens", n)) out.max_tokens = n;
    bool b = false;
    if (re_bool(j, "use_mock", b)) out.use_mock = b;
    if (re_int(j, "llm_daily_call_limit", n)) out.llm_daily_call_limit = n;
    if (json_extract_string_after_key(j, "workspace_root", s)) out.workspace_root = s;
    if (re_int(j, "git_worker_port", n)) out.git_worker_port = n;
    if (re_bool(j, "git_worker_autostart", b)) out.git_worker_autostart = b;
    if (json_extract_string_after_key(j, "sql_odbc_connection_string", s)) out.sql_odbc_connection_string = s;

    /* 仅有 glm-* 模型名却仍标注 anthropic 时，自动归为智谱，避免列表/API 路由不一致 */
    {
      std::string ml = out.model;
      for (char& c : ml) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      std::string prov_chk = out.llm_provider;
      for (char& c : prov_chk) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if ((prov_chk.empty() || prov_chk == "anthropic") && ml.find("glm") != std::string::npos)
        out.llm_provider = "zhipu";
      else if (prov_chk == "zhipu" && ml.find("claude") != std::string::npos)
        out.llm_provider = "anthropic";
    }

    std::string prov_lc = out.llm_provider;
    for (char& c : prov_lc) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (prov_lc == "zhipu") {
      if (out.api_host.empty() || out.api_host == "api.anthropic.com") out.api_host = "open.bigmodel.cn";
      if (out.api_path.empty() || out.api_path == "/v1/messages") out.api_path = "/api/paas/v4/chat/completions";
      std::string ml = out.model;
      for (char& c : ml) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (ml.find("claude") != std::string::npos) out.model = "glm-4";
    }
    return true;
  } catch (const std::exception& e) {
    error = std::string("解析配置失败: ") + e.what();
    return false;
  }
}

void save_example_config(const std::filesystem::path& path, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "无法创建目录: " + path.parent_path().string() + " — " + ec.message();
    return;
  }
  static const char* kBody = R"({
  "llm_provider": "anthropic",
  "api_key": "",
  "model": "claude-sonnet-4-20250514",
  "api_host": "api.anthropic.com",
  "api_path": "/v1/messages",
  "max_context_chars": 12000,
  "max_tokens": 4096,
  "use_mock": true,
  "llm_daily_call_limit": 0,
  "workspace_root": "",
  "git_worker_port": 47821,
  "git_worker_autostart": true,
  "sql_odbc_connection_string": "Driver={ODBC Driver 17 for SQL Server};Server=localhost;Database=CCT_CN;Trusted_Connection=yes;TrustServerCertificate=yes;",
  "comment": "llm_provider: anthropic（Claude）或 zhipu（智谱 GLM）。智谱示例：llm_provider=zhipu，api_key=智谱Key，model=glm-4，api_host=open.bigmodel.cn，api_path=/api/paas/v4/chat/completions。use_mock=false 且填写 api_key 后走真实 API（Windows WinHTTP）。llm_daily_call_limit>0 时仅约束免费档单日模型调用次数；为 0 时免费档不限，付费档使用内置各档位上限。workspace_root 填本机目录以启用 Web IDE 文件 API。sql_odbc_connection_string 为 SQL Server ODBC 连接串，Web 服务启动前必须配置，并需先执行 scripts/sql/schema.sql 建表。"
}
)";
  std::ofstream f(path);
  if (!f) {
    error = "无法写入: " + path.string();
    return;
  }
  f << kBody;
}

void apply_cli_workspace_override(AppConfig& cfg, const char* utf8_path_if_nonnull) {
  if (utf8_path_if_nonnull && utf8_path_if_nonnull[0]) cfg.workspace_root.assign(utf8_path_if_nonnull);
}

}  // namespace cct::util

