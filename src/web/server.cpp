#include "server.hpp"

#include "../llm/api.hpp"
#include "../util/component_name_utf8.hpp"
#include "../util/config.hpp"
#include "../util/json_minimal.hpp"
#include "../util/utf8_string.hpp"
#include "../storage/file_chat_store.hpp"
#include "../storage/file_component_store.hpp"
#include "../storage/file_token_billing.hpp"
#include "../storage/file_user_persistence.hpp"
#include "../storage/storage_helpers.hpp"
#include "../storage/storage_iface.hpp"
#include "../storage/token_billing_common.hpp"
#include "crypto.hpp"
#include <functional>
#include "http_util.hpp"
#include "git_worker_client.hpp"

extern "C" {
void* cct_sql_bundle_open(const char* conn_utf8, char* err, size_t err_cap);
void cct_sql_bundle_close(void* bundle);
cct::storage::IUserPersistence* cct_sql_bundle_users(void* bundle);
cct::storage::IChatPersistence* cct_sql_bundle_chats(void* bundle);
cct::storage::IComponentPersistence* cct_sql_bundle_components(void* bundle);
cct::storage::ITokenBillingPersistence* cct_sql_bundle_token_billing(void* bundle);
}

#ifndef _WIN32

namespace cct::web {

void run_http_server(std::uint16_t, const std::filesystem::path&, const std::filesystem::path&,
                     const cct::util::AppConfig&) {}

}

#else

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>
#include <mutex>
#include <cctype>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <chrono>
#include <ctime>
#include <regex>
#include <cstdio>
#pragma comment(lib, "ws2_32.lib")

namespace cct::web {

namespace {

struct Session {
  std::uint64_t user_id = 0;
  std::string username;
};

std::mutex g_sess_mu;
std::unordered_map<std::string, Session> g_sessions;

struct CaptchaEntry {
  std::string answer;
  std::uint64_t expires_ms = 0;
};
std::mutex g_captcha_mu;
std::unordered_map<std::string, CaptchaEntry> g_captchas;

static std::uint64_t steady_now_ms() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/** 对话补全上限：配置里默认 max_tokens 常偏小，多文件 CCT_WORKSPACE JSON 易被截断（截断后预览仅见红/绿条、保存为空）。 */
static void augment_chat_max_tokens_for_request(cct::util::AppConfig& cfg, bool has_workspace_bundle) {
  constexpr int k_chat_min_max_tokens = 8192;
  constexpr int k_chat_workspace_min_max_tokens = 16384;
  cfg.max_tokens = (std::max)(cfg.max_tokens, k_chat_min_max_tokens);
  if (has_workspace_bundle) cfg.max_tokens = (std::max)(cfg.max_tokens, k_chat_workspace_min_max_tokens);
}

static void captcha_prune_locked() {
  const std::uint64_t t = steady_now_ms();
  for (auto it = g_captchas.begin(); it != g_captchas.end();) {
    if (it->second.expires_ms < t)
      it = g_captchas.erase(it);
    else
      ++it;
  }
}

static std::string svg_captcha_4digits(const std::string& four) {
  std::ostringstream o;
  o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"168\" height=\"52\" viewBox=\"0 0 168 52\"><rect fill=\"#f5f5f7\" width=\"168\" height=\"52\" rx=\"12\" stroke=\"#d2d2d7\" stroke-width=\"1\"/>";
  o << "<text x=\"84\" y=\"34\" font-size=\"23\" font-family=\"ui-monospace,Menlo,Consolas,monospace\" font-weight=\"650\" fill=\"#1d1d1f\" text-anchor=\"middle\" letter-spacing=\"0.12em\">";
  for (char c : four) {
    if (c >= '0' && c <= '9') o << c;
  }
  o << "</text></svg>";
  return o.str();
}

static bool extract_json_string_array_simple(const std::string& json, const std::string& key,
                                             std::vector<std::string>& out) {
  out.clear();
  const std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) return false;
  p = json.find('[', p + needle.size());
  if (p == std::string::npos) return false;
  size_t i = p + 1;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
  while (i < json.size() && json[i] != ']') {
    if (json[i] != '"') return false;
    ++i;
    std::string s;
    while (i < json.size() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < json.size()) {
        char n = json[i + 1];
        if (n == 'n') {
          s += '\n';
          i += 2;
        } else if (n == 'r') {
          s += '\r';
          i += 2;
        } else if (n == 't') {
          s += '\t';
          i += 2;
        } else if (n == '\\' || n == '"') {
          s += n;
          i += 2;
        } else {
          s += json[i++];
        }
      } else
        s += json[i++];
    }
    if (i >= json.size() || json[i] != '"') return false;
    out.push_back(std::move(s));
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
    if (i < json.size() && json[i] == ',') ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
  }
  return true;
}

/** Mock / JSON 中的中文维度标题，须与用户所选维度 ID 一致 */
static std::string code_scan_dim_title_cn(const std::string& d) {
  if (d == "performance") return "性能与算法";
  if (d == "maintainability") return "可读与结构";
  if (d == "robustness") return "健壮性";
  if (d == "completeness") return "完整度";
  if (d == "security") return "安全性";
  return d;
}

static std::string code_scan_rules_for_dimensions(const std::vector<std::string>& dims) {
  std::ostringstream o;
  for (const auto& raw : dims) {
    std::string d = raw;
    for (char& c : d) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (d == "performance") {
      o << "### dimension:performance — 性能与算法（判定准则）\n"
           "- 时间与空间复杂度：是否存在可被替代的更低复杂度写法（如对向量频繁 erase(begin)、双重循环可哈希化为 O(n)）。\n"
           "- 热点路径：循环内重复构造对象、重复字符串拼接、可前移的不变计算。\n"
           "- I/O 与同步：是否在紧密循环中读写磁盘或持锁过久。\n"
           "- 数据结构：unordered_map vs map、vector 预留容量 reserve、不必要拷贝（应按常量引用传递大对象）。\n\n";
    } else if (d == "maintainability") {
      o << "### dimension:maintainability — 可读性与结构（判定准则）\n"
           "- 命名：标识符是否语义清晰；是否滥用缩写或与领域不符。\n"
           "- 长度与职责：函数是否过长（建议警惕超过 ~80 行且无分段）；类是否承担多重职责。\n"
           "- 重复代码：是否存在可复制粘贴片段应抽取为函数或模板。\n"
           "- 注释：复杂分支是否有简短意图说明；避免废话或与代码矛盾的注释。\n\n";
    } else if (d == "robustness") {
      o << "### dimension:robustness — 健壮性（判定准则）\n"
           "- 空指针与越界：解引用前是否保证有效；索引访问是否校验范围。\n"
           "- 错误路径：失败时返回值与日志是否合理；资源（文件句柄、锁）是否释放。\n"
           "- 输入校验：外部数据（用户输入、网络、文件）是否经过校验后再使用。\n"
           "- 并发：是否存在明显竞态、双重检查锁定错误或未保护的共享可变状态。\n\n";
    } else if (d == "completeness") {
      o << "### dimension:completeness — 完整度（判定准则）\n"
           "- TODO/FIXME/占位函数是否仍存在未完成逻辑。\n"
           "- 分支完整性：switch 是否遗漏枚举值；错误码是否覆盖。\n"
           "- API 契约：函数前置条件与返回值是否与调用方约定一致；缺文档的关键边界。\n\n";
    } else if (d == "security") {
      o << "### dimension:security — 安全性（判定准则）\n"
           "- 注入：SQL/OS 命令/HTML 拼接是否使用参数化或转义。\n"
           "- 隐私：密钥、Token、口令是否硬编码或写入日志。\n"
           "- 危险 API：eval、不加限制的 Deserialize、明文传输敏感数据。\n"
           "- 权限：服务端路径拼接是否仍约束在工作区内（路径穿越）。\n\n";
    }
  }
  return o.str();
}

static bool valid_thread_id_str(const std::string& id) {
  if (id.empty() || id.size() > 48) return false;
  for (unsigned char uc : id) {
    char c = static_cast<char>(uc);
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
  }
  return true;
}

static std::string parse_thread_id_json(const std::string& body) {
  std::string tid;
  if (!cct::util::json_extract_string_after_key(body, "thread_id", tid) || tid.empty()) tid = "main";
  if (!valid_thread_id_str(tid)) tid = "main";
  return tid;
}

static std::string messages_to_json_array(const std::vector<cct::llm::ChatMessage>& h) {
  std::ostringstream o;
  o << '[';
  for (size_t i = 0; i < h.size(); ++i) {
    if (i) o << ',';
    o << "{\"role\":\"" << cct::util::json_escape_string(h[i].role) << "\",\"content\":\""
      << cct::util::json_escape_string(h[i].content) << "\"}";
  }
  o << ']';
  return o.str();
}

static std::uint64_t next_thread_ordinal(const std::vector<cct::storage::ChatThreadRow>& rows) {
  std::uint64_t m = 0;
  for (const auto& r : rows)
    if (r.ordinal > m) m = r.ordinal;
  return m + 1;
}

static std::string norm_workspace_rel_path(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  while (!p.empty() && (p.front() == '/' || p.front() == '.')) {
    if (p.front() == '.' && (p.size() == 1 || p[1] != '/')) break;
    p.erase(p.begin());
    if (!p.empty() && p.front() == '/') p.erase(p.begin());
  }
  std::vector<std::string> segs;
  std::stringstream ss(p);
  std::string part;
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    if (part == "." || part == ".." || part.find(':') != std::string::npos || part.find('\t') != std::string::npos)
      return {};
    segs.push_back(part);
  }
  std::ostringstream out;
  for (size_t i = 0; i < segs.size(); ++i) {
    if (i) out << '/';
    out << segs[i];
  }
  return out.str();
}

static bool looks_like_workspace_file_segment(const std::string& seg) {
  const auto dot = seg.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= seg.size()) return false;
  const std::string ext = seg.substr(dot + 1);
  if (ext.empty() || ext.size() > 15) return false;
  for (unsigned char uc : ext) {
    if (!std::isalnum(static_cast<unsigned char>(uc))) return false;
  }
  return true;
}

static std::string derive_workspace_anchor_from_rel_path(const std::string& rel_path) {
  const std::string n = norm_workspace_rel_path(rel_path);
  if (n.empty()) return {};
  const size_t last_slash = n.rfind('/');
  const std::string last = last_slash == std::string::npos ? n : n.substr(last_slash + 1);
  if (looks_like_workspace_file_segment(last)) {
    if (last_slash == std::string::npos) return {};
    return n.substr(0, last_slash);
  }
  return n;
}

static std::string derive_workspace_anchor_from_request(const std::string& editor_path,
                                                        const std::string& workspace_bundle) {
  std::string anchor = derive_workspace_anchor_from_rel_path(editor_path);
  if (!anchor.empty()) return anchor;
  std::string bundle_path;
  if (cct::util::json_extract_string_after_key(workspace_bundle, "path", bundle_path)) {
    anchor = derive_workspace_anchor_from_rel_path(bundle_path);
  }
  return anchor;
}

static void apply_thread_workspace_anchor(std::vector<cct::storage::ChatThreadRow>& rows, const std::string& thread_id,
                                          const std::string& anchor) {
  if (anchor.empty()) return;
  for (auto& row : rows) {
    if (row.id != thread_id) continue;
    if (row.workspace_anchor != anchor) {
      row.workspace_anchor = anchor;
      row.updated++;
    }
    return;
  }
}

std::string json_err(const std::string& msg) {
  return std::string("{\"ok\":false,\"error\":\"") + cct::util::json_escape_string(msg) + "\"}";
}

/** 带业务 code 字段，供前端区分额度类错误 */
std::string json_err_msg_code(const std::string& msg, const char* code) {
  return std::string("{\"ok\":false,\"error\":\"") + cct::util::json_escape_string(msg) + "\",\"code\":\"" +
         cct::util::json_escape_string(code) + "\"}";
}

std::string json_ok(const std::string& inner) { return std::string("{\"ok\":true") + inner + "}"; }

std::string new_session_token() {
  std::vector<unsigned char> r;
  if (!cct::web::crypto::random_bytes(r, 32)) return {};
  return cct::web::crypto::bytes_to_hex(r.data(), r.size());
}

bool valid_component_name(const std::string& n) { return cct::util::valid_component_name_utf8(n); }

/** HTTP 请求行里的 path 不经百分号解码；前端对 UTF-8 slug 使用 encodeURIComponent（如 %E6%AF%95…），须还原后再查库 */
static std::string uri_percent_decode_utf8(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= '0' && uc <= '9') return static_cast<int>(uc - '0');
        if (uc >= 'a' && uc <= 'f') return static_cast<int>(uc - 'a' + 10);
        if (uc >= 'A' && uc <= 'F') return static_cast<int>(uc - 'A' + 10);
        return -1;
      };
      int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 3;
        continue;
      }
    }
    out.push_back(s[i++]);
  }
  return out;
}

/** /api/chat 可选 model 字段：仅允许安全字符，且与当前 provider 前缀一致 */
bool valid_chat_model_id(const std::string& m) {
  if (m.empty() || m.size() > 64) return false;
  for (unsigned char uc : m) {
    char c = static_cast<char>(uc);
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}

bool allowed_chat_model_for_provider(const std::string& prov_lc, const std::string& m) {
  if (!valid_chat_model_id(m)) return false;
  if (prov_lc == "zhipu") return m.size() >= 4 && m.compare(0, 4, "glm-") == 0;
  if (prov_lc == "anthropic") return m.size() >= 6 && m.compare(0, 6, "claude") == 0;
  return false;
}

/** 与 file_chat_store.cpp safe_thread_stem 一致，用于删除 users/<uid>/chats/m_<stem>.json */
static std::string safe_thread_file_stem(const std::string& tid) {
  std::string s;
  s.reserve(tid.size());
  for (char c : tid) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') s += c;
    else s += '_';
  }
  if (s.empty()) s = "t";
  return s;
}

static bool re_int_json_body(const std::string& j, const char* key, int& out) {
  std::string pat = std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+)";
  std::regex re(pat);
  std::smatch m;
  if (!std::regex_search(j, m, re)) return false;
  try {
    out = std::stoi(m[1].str());
  } catch (...) {
    return false;
  }
  return true;
}

struct LlmUsageSnap {
  std::string day;
  int calls_today = 0;
  int total_calls = 0;
};

static std::mutex g_llm_usage_mu;

static std::filesystem::path llm_usage_json_path(const std::filesystem::path& data_dir, std::uint64_t uid) {
  return data_dir / "users" / std::to_string(uid) / "llm_usage.json";
}

static std::string local_date_yyyy_mm_dd() {
  std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_s(&lt, &t);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  return buf;
}

static void load_llm_usage_raw(const std::filesystem::path& path, LlmUsageSnap& out) {
  out = {};
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  std::stringstream buf;
  buf << in.rdbuf();
  std::string j = buf.str();
  (void)cct::util::json_extract_string_after_key(j, "day", out.day);
  int n = 0;
  if (re_int_json_body(j, "calls_today", n)) out.calls_today = n;
  n = 0;
  if (re_int_json_body(j, "total_calls", n)) out.total_calls = n;
}

static void rollover_llm_usage_day(LlmUsageSnap& s) {
  const std::string today = local_date_yyyy_mm_dd();
  if (s.day != today) {
    s.day = today;
    s.calls_today = 0;
  }
}

static void save_llm_usage_raw(const std::filesystem::path& path, const LlmUsageSnap& s) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return;
  out << "{\"day\":\"" << cct::util::json_escape_string(s.day) << "\",\"calls_today\":" << s.calls_today
      << ",\"total_calls\":" << s.total_calls << "}\n";
}

static LlmUsageSnap peek_llm_usage_for_me(const std::filesystem::path& data_dir, std::uint64_t uid) {
  std::lock_guard<std::mutex> lk(g_llm_usage_mu);
  LlmUsageSnap s;
  load_llm_usage_raw(llm_usage_json_path(data_dir, uid), s);
  rollover_llm_usage_day(s);
  return s;
}

/** false = 超过单日限额（未递增）；true = 允许并已递增 */
static bool try_consume_llm_call_quota(const std::filesystem::path& data_dir, std::uint64_t uid, int daily_limit,
                                       std::string& deny_msg, LlmUsageSnap& snap_after) {
  std::lock_guard<std::mutex> lk(g_llm_usage_mu);
  const auto path = llm_usage_json_path(data_dir, uid);
  LlmUsageSnap s;
  load_llm_usage_raw(path, s);
  rollover_llm_usage_day(s);
  if (daily_limit > 0 && s.calls_today >= daily_limit) {
    deny_msg = "今日模型调用次数已达上限（" + std::to_string(daily_limit) + "）";
    snap_after = s;
    return false;
  }
  s.calls_today++;
  s.total_calls++;
  save_llm_usage_raw(path, s);
  snap_after = s;
  return true;
}

/** 成功完成 LLM 调用后累加 analytics 用计数（不按日限额拦截） */
static void record_llm_call_success_for_analytics(const std::filesystem::path& data_dir, std::uint64_t uid) {
  std::lock_guard<std::mutex> lk(g_llm_usage_mu);
  const auto path = llm_usage_json_path(data_dir, uid);
  LlmUsageSnap s;
  load_llm_usage_raw(path, s);
  rollover_llm_usage_day(s);
  s.calls_today++;
  s.total_calls++;
  save_llm_usage_raw(path, s);
}

static std::int64_t billing_delta_from_llm_usage(int pt, int ct, std::int64_t min_tokens, std::size_t approx_utf8_bytes) {
  std::int64_t u = 0;
  if (pt >= 0 && ct >= 0) u = static_cast<std::int64_t>(pt) + static_cast<std::int64_t>(ct);
  if (u <= 0) u = static_cast<std::int64_t>(approx_utf8_bytes / 4 + 64);
  return (std::max)(min_tokens, u);
}

static bool tier_daily_call_blocked(const std::filesystem::path& data_dir, const std::string& tier_norm, std::uint64_t uid,
                                    int cfg_llm_daily_call_limit, std::string& deny_msg) {
  const int cap = cct::storage::billing_effective_daily_llm_calls(tier_norm, cfg_llm_daily_call_limit);
  if (cap <= 0) return false;
  const LlmUsageSnap lu = peek_llm_usage_for_me(data_dir, uid);
  if (lu.calls_today >= cap) {
    deny_msg = "今日模型调用次数已达上限（" + std::to_string(cap) + "），请明日再试或升级订阅。";
    return true;
  }
  return false;
}

static std::string billing_plans_json_inner() {
  std::ostringstream o;
  o << ",\"plans\":["
       "{\"tier\":\"free\",\"name\":\"免费版\",\"priceCny\":0,\"tokenQuota\":"
    << cct::storage::billing_token_quota_for_tier("free") << ",\"dailyCallCap\":0"
    << ",\"features\":[\"核心模型\",\"有限消息与上传\",\"有限图片与记忆\"]},"
       "{\"tier\":\"go\",\"name\":\"Go\",\"priceCny\":58,\"tokenQuota\":"
    << cct::storage::billing_token_quota_for_tier("go") << ",\"dailyCallCap\":"
    << cct::storage::billing_daily_llm_calls_cap_for_tier("go")
    << ",\"features\":[\"核心模型\",\"更多消息与上传\",\"更多图片生成\",\"扩展语音额度\"]},"
       "{\"tier\":\"plus\",\"name\":\"Plus\",\"priceCny\":138,\"tokenQuota\":"
    << cct::storage::billing_token_quota_for_tier("plus") << ",\"dailyCallCap\":"
    << cct::storage::billing_daily_llm_calls_cap_for_tier("plus")
    << ",\"popular\":true,\"features\":[\"高级模型\",\"更高消息与上传额度\",\"深度研究\",\"编码智能体\"]},"
       "{\"tier\":\"pro\",\"name\":\"Pro\",\"priceCny\":698,\"tokenQuota\":"
    << cct::storage::billing_token_quota_for_tier("pro") << ",\"dailyCallCap\":"
    << cct::storage::billing_daily_llm_calls_cap_for_tier("pro")
    << ",\"features\":[\"相较 Plus 更高可用额度\",\"前沿模型\",\"最高级别研究\"]}]";
  return o.str();
}

static std::mutex g_analytics_mu;

static std::filesystem::path analytics_events_jsonl_path(const std::filesystem::path& data_dir, std::uint64_t uid) {
  return data_dir / "users" / std::to_string(uid) / "analytics_events.jsonl";
}

static std::string analytics_timestamp_iso_local() {
  std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_s(&lt, &t);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
  return std::string(buf);
}

static void analytics_append_jsonl(const std::filesystem::path& data_dir, std::uint64_t uid, const std::string& line) {
  std::lock_guard<std::mutex> lk(g_analytics_mu);
  std::error_code ec;
  const auto path = analytics_events_jsonl_path(data_dir, uid);
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return;
  out << line << "\n";
}

static void analytics_emit_chat_completion(const std::filesystem::path& data_dir, std::uint64_t uid,
                                           const std::string& thread_id, const std::string& agent_pick,
                                           const std::string& skill_pick, const std::string& command_pick,
                                           bool has_ws_bundle, bool has_editor_ctx, const std::string& model_pick,
                                           bool ok, std::uint64_t latency_ms, int pt, int ct, int tt) {
  std::ostringstream o;
  o << "{\"t\":\"" << analytics_timestamp_iso_local() << "\",\"k\":\"chat\""
    << ",\"th\":\"" << cct::util::json_escape_string(thread_id) << "\""
    << ",\"ok\":" << (ok ? 1 : 0) << ",\"ms\":" << latency_ms;
  if (!agent_pick.empty()) o << ",\"ag\":\"" << cct::util::json_escape_string(agent_pick) << "\"";
  if (!skill_pick.empty()) o << ",\"sk\":\"" << cct::util::json_escape_string(skill_pick) << "\"";
  if (!command_pick.empty()) o << ",\"cmd\":\"" << cct::util::json_escape_string(command_pick) << "\"";
  if (!model_pick.empty()) o << ",\"md\":\"" << cct::util::json_escape_string(model_pick) << "\"";
  if (pt >= 0) o << ",\"pt\":" << pt;
  if (ct >= 0) o << ",\"ct\":" << ct;
  if (tt >= 0) o << ",\"tt\":" << tt;
  o << ",\"wb\":" << (has_ws_bundle ? 1 : 0) << ",\"ed\":" << (has_editor_ctx ? 1 : 0) << "}";
  analytics_append_jsonl(data_dir, uid, o.str());
}

static void analytics_emit_scan_completion(const std::filesystem::path& data_dir, std::uint64_t uid, int paths_count,
                                           int dims_count, const std::string& model_logged, bool ok,
                                           std::uint64_t latency_ms, int pt, int ct, int tt) {
  std::ostringstream o;
  o << "{\"t\":\"" << analytics_timestamp_iso_local() << "\",\"k\":\"scan\",\"pc\":" << paths_count << ",\"dc\":"
    << dims_count << ",\"ok\":" << (ok ? 1 : 0) << ",\"ms\":" << latency_ms;
  if (!model_logged.empty()) o << ",\"md\":\"" << cct::util::json_escape_string(model_logged) << "\"";
  if (pt >= 0) o << ",\"pt\":" << pt;
  if (ct >= 0) o << ",\"ct\":" << ct;
  if (tt >= 0) o << ",\"tt\":" << tt;
  o << "}";
  analytics_append_jsonl(data_dir, uid, o.str());
}

static void analytics_emit_ws_event(const std::filesystem::path& data_dir, std::uint64_t uid,
                                    const std::string& op, const std::string& rel_path, int byte_count) {
  std::ostringstream o;
  o << "{\"t\":\"" << analytics_timestamp_iso_local() << "\",\"k\":\"ws\""
    << ",\"op\":\"" << cct::util::json_escape_string(op) << "\""
    << ",\"p\":\"" << cct::util::json_escape_string(rel_path) << "\",\"bc\":" << byte_count << "}";
  analytics_append_jsonl(data_dir, uid, o.str());
}

static bool analytics_re_str(const std::string& line, const char* key, std::string& out) {
  std::string pat = std::string("\"") + key + "\"\\s*:\\s*\"([^\"]*)\"";
  std::regex re(pat);
  std::smatch m;
  if (!std::regex_search(line, m, re)) return false;
  out = m[1].str();
  return true;
}

static std::vector<std::string> analytics_window_days(int n) {
  std::vector<std::string> days;
  for (int i = n - 1; i >= 0; --i) {
    std::time_t tt = std::time(nullptr) - static_cast<std::time_t>(i) * 86400;
    std::tm lt{};
    localtime_s(&lt, &tt);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    days.push_back(buf);
  }
  return days;
}

static std::string analytics_build_summary_inner(const std::filesystem::path& data_dir, std::uint64_t uid,
                                                 const LlmUsageSnap& lu, int daily_limit,
                                                 std::size_t threads_storage_count,
                                                 const std::string& subscription_tier, std::int64_t token_quota,
                                                 std::int64_t tokens_consumed) {
  constexpr int k_days = 14;
  std::unordered_map<std::string, int> day_llm;
  std::unordered_map<std::string, long long> day_tokens;
  int chat_n = 0;
  int scan_n = 0;
  int chat_ok_n = 0;
  int chat_fail_n = 0;
  int scan_ok_n = 0;
  int scan_fail_n = 0;
  long long sum_chat_ms = 0;
  int chat_lat_n = 0;
  long long sum_scan_ms = 0;
  int scan_lat_n = 0;
  long long sum_prompt_tok = 0;
  long long sum_compl_tok = 0;
  long long sum_total_tok = 0;
  long long inferred_total_tokens = 0;
  int bundle_yes = 0;
  int editor_yes = 0;
  std::unordered_map<std::string, int> tool_labels;
  std::unordered_map<std::string, int> model_labels;
  std::unordered_set<std::string> threads_seen;
  long long scan_paths_sum = 0;
  std::size_t lines_read = 0;
  int ws_writes = 0;
  int ws_mkdirs = 0;
  long long ws_bytes = 0;
  std::unordered_map<std::string, int> ws_path_hits;

  const auto window = analytics_window_days(k_days);
  std::unordered_set<std::string> window_day(window.begin(), window.end());

  const auto path = analytics_events_jsonl_path(data_dir, uid);
  std::ifstream fin(path, std::ios::binary | std::ios::ate);
  std::string content;
  if (fin) {
    std::streamoff sz64 = fin.tellg();
    constexpr std::streamoff cap = static_cast<std::streamoff>(4 * 1024 * 1024);
    if (sz64 > 0 && sz64 <= cap) {
      fin.seekg(0);
      content.assign(std::istreambuf_iterator<char>(fin), std::istreambuf_iterator<char>());
    } else if (sz64 > cap) {
      fin.seekg(sz64 - cap);
      content.assign(std::istreambuf_iterator<char>(fin), std::istreambuf_iterator<char>());
      const auto cut = content.find('\n');
      if (cut != std::string::npos) content.erase(0, cut + 1);
    }
  }

  for (std::size_t pos = 0; pos < content.size();) {
    const std::size_t nl = content.find('\n', pos);
    std::string line =
        nl == std::string::npos ? content.substr(pos) : content.substr(pos, nl - pos);
    pos = nl == std::string::npos ? content.size() : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    lines_read++;
    std::string k;
    if (!analytics_re_str(line, "k", k)) continue;
    std::string ts;
    (void)analytics_re_str(line, "t", ts);
    const std::string day = ts.size() >= 10 ? ts.substr(0, 10) : "";
    if (!day.empty() && (k == "chat" || k == "scan")) day_llm[day]++;
    if (!day.empty() && window_day.count(day) && (k == "chat" || k == "scan")) {
      int pt = -1, ct = -1, tt = -1;
      (void)re_int_json_body(line, "pt", pt);
      (void)re_int_json_body(line, "ct", ct);
      (void)re_int_json_body(line, "tt", tt);
      long long tot = -1;
      if (tt >= 0)
        tot = tt;
      else if (pt >= 0 && ct >= 0)
        tot = static_cast<long long>(pt) + ct;
      else if (pt >= 0)
        tot = pt;
      else if (ct >= 0)
        tot = ct;
      if (tot >= 0) {
        day_tokens[day] += tot;
        inferred_total_tokens += tot;
      }
      if (pt >= 0) sum_prompt_tok += pt;
      if (ct >= 0) sum_compl_tok += ct;
      if (tt >= 0) sum_total_tok += tt;
    }

    if (k == "chat") {
      chat_n++;
      std::string th;
      if (analytics_re_str(line, "th", th) && !th.empty()) threads_seen.insert(th);
      std::string ag, sk, cmd;
      if (analytics_re_str(line, "ag", ag) && !ag.empty()) tool_labels[std::string("Agent · ") + ag]++;
      if (analytics_re_str(line, "sk", sk) && !sk.empty()) tool_labels[std::string("Skill · ") + sk]++;
      if (analytics_re_str(line, "cmd", cmd) && !cmd.empty()) tool_labels[std::string("Command · ") + cmd]++;
      int wb = 0, ed = 0;
      if (re_int_json_body(line, "wb", wb) && wb) bundle_yes++;
      if (re_int_json_body(line, "ed", ed) && ed) editor_yes++;
      std::string md;
      if (analytics_re_str(line, "md", md) && !md.empty()) model_labels[md]++;
      int okf = 1;
      if (re_int_json_body(line, "ok", okf)) {
        if (okf)
          chat_ok_n++;
        else
          chat_fail_n++;
      } else {
        chat_ok_n++;
      }
      int ms = -1;
      if (re_int_json_body(line, "ms", ms) && ms >= 0 && okf) {
        sum_chat_ms += ms;
        chat_lat_n++;
      }
    } else if (k == "scan") {
      scan_n++;
      int pc = 0;
      if (re_int_json_body(line, "pc", pc)) scan_paths_sum += pc;
      std::string md;
      if (analytics_re_str(line, "md", md) && !md.empty()) model_labels[md]++;
      int okf = 1;
      if (re_int_json_body(line, "ok", okf)) {
        if (okf)
          scan_ok_n++;
        else
          scan_fail_n++;
      } else {
        scan_ok_n++;
      }
      int ms = -1;
      if (re_int_json_body(line, "ms", ms) && ms >= 0 && okf) {
        sum_scan_ms += ms;
        scan_lat_n++;
      }
    } else if (k == "ws") {
      std::string opv, pv;
      analytics_re_str(line, "op", opv);
      analytics_re_str(line, "p", pv);
      int bc = 0;
      (void)re_int_json_body(line, "bc", bc);
      if (opv == "mkdir") {
        ws_mkdirs++;
      } else {
        ws_writes++;
        ws_bytes += static_cast<long long>(bc);
        if (!pv.empty()) ws_path_hits[pv]++;
      }
    }
  }

  const int denom_chat = chat_n > 0 ? chat_n : 1;
  const int pct_wb = static_cast<int>((100LL * bundle_yes) / denom_chat);
  const int pct_ed = static_cast<int>((100LL * editor_yes) / denom_chat);
  const double avg_scan_paths =
      scan_n > 0 ? static_cast<double>(scan_paths_sum) / static_cast<double>(scan_n) : 0.0;
  char avg_buf[48];
  std::snprintf(avg_buf, sizeof(avg_buf), "%.1f", avg_scan_paths);

  const double chat_avg_ms = chat_lat_n > 0 ? static_cast<double>(sum_chat_ms) / chat_lat_n : 0.0;
  const double scan_avg_ms = scan_lat_n > 0 ? static_cast<double>(sum_scan_ms) / scan_lat_n : 0.0;
  char chat_avg_buf[48], scan_avg_buf[48];
  std::snprintf(chat_avg_buf, sizeof(chat_avg_buf), "%.0f", chat_avg_ms);
  std::snprintf(scan_avg_buf, sizeof(scan_avg_buf), "%.0f", scan_avg_ms);
  const int chat_ok_pct = chat_n > 0 ? static_cast<int>((100LL * chat_ok_n) / chat_n) : 100;
  const int scan_ok_pct = scan_n > 0 ? static_cast<int>((100LL * scan_ok_n) / scan_n) : 100;

  std::vector<std::pair<std::string, int>> tool_vec(tool_labels.begin(), tool_labels.end());
  std::sort(tool_vec.begin(), tool_vec.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  if (tool_vec.size() > 12) tool_vec.resize(12);

  std::vector<std::pair<std::string, int>> model_vec(model_labels.begin(), model_labels.end());
  std::sort(model_vec.begin(), model_vec.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  if (model_vec.size() > 8) model_vec.resize(8);

  std::vector<std::pair<std::string, int>> ws_vec(ws_path_hits.begin(), ws_path_hits.end());
  std::sort(ws_vec.begin(), ws_vec.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  if (ws_vec.size() > 8) ws_vec.resize(8);

  std::ostringstream o;
  o << ",\"llmDailyLimit\":" << daily_limit << ",\"llmCallsToday\":" << lu.calls_today << ",\"llmTotalCalls\":"
    << lu.total_calls << ",\"threadsStored\":" << threads_storage_count << ",\"threadsActiveInLog\":"
    << threads_seen.size() << ",\"analyticsPeriodDays\":" << k_days << ",\"eventsLinesRead\":" << lines_read
    << ",\"subscriptionTier\":\"" << cct::util::json_escape_string(subscription_tier) << "\""
    << ",\"tokenQuota\":" << token_quota << ",\"tokensConsumed\":" << tokens_consumed << ",\"tokensRemaining\":"
    << (std::max)(static_cast<std::int64_t>(0), token_quota - tokens_consumed) << ",\"dailyCalls\":[";
  for (std::size_t i = 0; i < window.size(); ++i) {
    if (i) o << ",";
    const std::string& d = window[i];
    int cn = 0;
    const auto it = day_llm.find(d);
    if (it != day_llm.end()) cn = it->second;
    o << "{\"day\":\"" << d << "\",\"n\":" << cn << "}";
  }
  o << "],\"dailyTokens\":[";
  for (std::size_t i = 0; i < window.size(); ++i) {
    if (i) o << ",";
    const std::string& d = window[i];
    long long tn = 0;
    const auto it = day_tokens.find(d);
    if (it != day_tokens.end()) tn = it->second;
    o << "{\"day\":\"" << d << "\",\"n\":" << tn << "}";
  }
  o << "],\"kindSplit\":{\"chat\":" << chat_n << ",\"scan\":" << scan_n << "}";
  o << ",\"reliability\":{\"chatOkPct\":" << chat_ok_pct << ",\"scanOkPct\":" << scan_ok_pct
    << ",\"chatFail\":" << chat_fail_n << ",\"scanFail\":" << scan_fail_n << "}";
  o << ",\"tokenTotals\":{\"prompt\":" << sum_prompt_tok << ",\"completion\":" << sum_compl_tok
    << ",\"totalFromApi\":" << sum_total_tok << ",\"inferredTotal\":" << inferred_total_tokens << "}";
  o << ",\"latency\":{\"chatAvgMs\":" << chat_avg_buf << ",\"chatSamples\":" << chat_lat_n << ",\"scanAvgMs\":"
    << scan_avg_buf << ",\"scanSamples\":" << scan_lat_n << "}";
  o << ",\"workspaceWrites\":{\"writes\":" << ws_writes << ",\"mkdirs\":" << ws_mkdirs << ",\"bytes\":" << ws_bytes
    << "}";
  o << ",\"productivity\":{\"chatWithWorkspaceBundlePct\":" << pct_wb << ",\"chatWithEditorCtxPct\":" << pct_ed
    << ",\"avgScanPathsPerRun\":" << avg_buf << "}";
  o << ",\"toolPickHits\":[";
  for (std::size_t i = 0; i < tool_vec.size(); ++i) {
    if (i) o << ",";
    o << "{\"label\":\"" << cct::util::json_escape_string(tool_vec[i].first) << "\",\"n\":" << tool_vec[i].second
      << "}";
  }
  o << "],\"modelHits\":[";
  for (std::size_t i = 0; i < model_vec.size(); ++i) {
    if (i) o << ",";
    o << "{\"label\":\"" << cct::util::json_escape_string(model_vec[i].first) << "\",\"n\":" << model_vec[i].second
      << "}";
  }
  o << "],\"workspacePathHits\":[";
  for (std::size_t i = 0; i < ws_vec.size(); ++i) {
    if (i) o << ",";
    o << "{\"label\":\"" << cct::util::json_escape_string(ws_vec[i].first) << "\",\"n\":" << ws_vec[i].second << "}";
  }
  o << "]";
  return o.str();
}

static void sanitize_thread_rename_title(std::string& t) {
  for (char& c : t) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  while (!t.empty() && t.front() == ' ') t.erase(t.begin());
  while (!t.empty() && t.back() == ' ') t.pop_back();
  t = cct::util::utf8_ellipsis_prefix_chars(t, 120);
}

bool safe_rel_path(const std::filesystem::path& rel) {
  for (const auto& part : rel) {
    if (part == ".." || part == ".") return false;
  }
  return true;
}

static std::string sanitize_export_folder_label(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  for (char& c : s) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 32) {
      c = '_';
      continue;
    }
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  }
  std::string collapsed;
  collapsed.reserve(s.size());
  bool prev_us = false;
  for (char c : s) {
    if (c == '_') {
      if (prev_us) continue;
      prev_us = true;
    } else
      prev_us = false;
    collapsed.push_back(c);
  }
  s.swap(collapsed);
  if (s.empty()) s = "CCT_Export";
  if (s.size() > 72) s = cct::util::utf8_safe_truncate_bytes(s, 72);
  return s;
}

/** 与 /api/workspace 使用相同根路径：已配置 workspace_root 则用之，否则回退 D:\\ */
static bool resolve_workspace_root_canonical(const cct::util::AppConfig& cfg, std::filesystem::path& out_can,
                                             std::string& err) {
  std::error_code ec;
  std::filesystem::path root_in;
  if (cfg.workspace_root.empty()) {
    root_in = std::filesystem::path("D:\\");
    std::filesystem::create_directories(root_in, ec);
  } else {
    /** cfg.workspace_root 为 UTF-8； MSVC path(string) 会按系统 ANSI，中文路径会错码 */
    root_in = std::filesystem::u8path(cfg.workspace_root);
    if (root_in.is_relative()) root_in = std::filesystem::current_path() / root_in;
  }
  out_can = std::filesystem::weakly_canonical(root_in, ec);
  if (ec || !std::filesystem::is_directory(out_can)) {
    err = "工作区根目录无效或不可访问";
    return false;
  }
  return true;
}

/** 将「当前打开文件」与「工作区多文件快照」注入到待发送的最后一条 user 消息前（供智谱 / Claude 理解项目） */
static void augment_hist_send_user_tail(std::vector<cct::llm::ChatMessage>& hist_send, const cct::util::AppConfig& cfg,
                                       const std::string& editor_path, const std::string& editor_content, bool has_editor,
                                       const std::string& workspace_bundle, bool has_ws) {
  if (hist_send.empty() || hist_send.back().role != "user") return;
  if (!has_editor && !has_ws) return;
  const std::size_t cap = static_cast<std::size_t>((std::max)(4096u, static_cast<unsigned>(cfg.max_context_chars)));
  std::string prefix;
  if (has_editor && !editor_content.empty()) {
    std::string slice = editor_content;
    if (slice.size() > cap / 2) slice.resize(cap / 2);
    std::string p = editor_path.empty() ? std::string("(未命名)") : editor_path;
    prefix += std::string("【当前聚焦文件: ") + p +
             "】\n以下为该文件内容（请结合下方工作区快照整体理解）：\n```\n" + slice + "\n```\n\n";
  }
  if (has_ws && !workspace_bundle.empty()) {
    std::string wb = workspace_bundle;
    const std::size_t budget = cap > prefix.size() + 1200 ? cap - prefix.size() - 1200 : 4000;
    if (wb.size() > budget) wb = wb.substr(0, budget) + "\n…（工作区快照因长度上限已截断）\n";
    prefix += "【工作区多文件快照（JSON：顶层 files 为数组，每项含 path 与 content；path 相对工作区根）】\n";
    prefix += wb;
    prefix +=
        "\n\n你在修改工作区文件后，必须在整段回复的**最后**输出可解析的机器块（任选其一，推荐第一种）：\n"
        "1) 单独一行 `CCT_WORKSPACE:` 后紧跟一行 JSON（不要包在 ``` 里）：\n"
        "   {\"writes\":[{\"path\":\"相对路径\",\"content\":\"该文件完整新内容\"}]}\n"
        "2) 或在文末用 ```json 围栏包住与上相同的整段 JSON（前端亦可解析）。\n"
        "3) 或在文末直接输出裸的 `{\"writes\":[...]}` 整对象（勿在 content 内嵌未转义换行）。\n"
        "writes 可含多项；只列出你改动或新建的文件；未改动的路径不要出现。\n"
        "若 path 在工作区根下尚不存在，将自动创建父目录并新建该文件（整文件写入）。\n"
        "**（补充）** 前端会从正文中的 Markdown ```代码围栏``` 自动生成「预览写盘」，不要求你只输出 JSON；为精确文件名建议在围栏首行写 "
        "`// path: 相对路径`（相对workspace根）。若要删除无用文件：在文末追加一行 `CCT_DELETE:` 后接 ```json `[\"相对路径\"]` 或 "
        "`{\"paths\":[\"...\"]}`，用户确认后才会删除。\n"
        "**（多分文件）** 若需要多个源文件（例如 `*.cpp` 与 `*.h`）：每个文件单独用一个围栏块；或在 `writes` JSON 中包含多条 `path`，每条对应完整内容。\n"
        "**（重要）** 智谱等若区分「思考」与「最终回答」：上述 JSON **必须出现在最终可见回答里**（用户能直接看到的那段正文），"
        "不要只写在思考过程里；也不要仅在自然语言里声称「已修复」却省略 JSON，否则磁盘不会变化。\n\n";
  }
  hist_send.back().content = prefix + "【用户输入】\n" + hist_send.back().content;
}

constexpr std::uintmax_t kWorkspaceMaxFile = static_cast<std::uintmax_t>(2) * 1024 * 1024;

/** 在发往模型的副本上，把组件工作室里配置的 Agent / Skill / Command 正文插到本条 user 消息最前 */
static void inject_user_agent_skill_prefix(std::vector<cct::llm::ChatMessage>& hist_send,
                                           cct::storage::IComponentPersistence& comps, std::uint64_t user_id,
                                           const std::string& agent_name, const std::string& skill_name,
                                           const std::string& command_name) {
  if (hist_send.empty() || hist_send.back().role != "user") return;
  std::string agent_body, skill_body, command_body;
  std::string ign;
  if (!agent_name.empty()) (void)comps.get_content(user_id, "agents", agent_name, agent_body, ign);
  if (!skill_name.empty()) (void)comps.get_content(user_id, "skills", skill_name, skill_body, ign);
  if (!command_name.empty()) (void)comps.get_content(user_id, "commands", command_name, command_body, ign);
  if (agent_body.empty() && skill_body.empty() && command_body.empty()) return;
  std::string block;
  if (!agent_body.empty())
    block += std::string("【角色 Agent: ") + agent_name + "】\n" + agent_body + "\n\n";
  if (!skill_body.empty())
    block += std::string("【技能 Skill: ") + skill_name + "】\n" + skill_body + "\n\n";
  if (!command_body.empty())
    block += std::string("【指令 Command: ") + command_name + "】\n" + command_body + "\n\n";
  hist_send.back().content = block + hist_send.back().content;
}

bool query_string_param(const std::string& q, const char* key, std::string& out) {
  const std::string prefix = std::string(key) + "=";
  size_t pos = 0;
  while (pos < q.size()) {
    size_t amp = q.find('&', pos);
    std::string part = amp == std::string::npos ? q.substr(pos) : q.substr(pos, amp - pos);
    if (part.size() >= prefix.size() && part.compare(0, prefix.size(), prefix) == 0) {
      std::string raw = part.substr(prefix.size());
      out.clear();
      for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
          auto hx = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          int h = hx(raw[i + 1]), l = hx(raw[i + 2]);
          if (h >= 0 && l >= 0) {
            out.push_back(static_cast<char>((h << 4) | l));
            i += 2;
            continue;
          }
        }
        if (raw[i] == '+')
          out.push_back(' ');
        else
          out.push_back(raw[i]);
      }
      return true;
    }
    pos = amp == std::string::npos ? q.size() : amp + 1;
  }
  return false;
}

bool path_is_under_root(const std::filesystem::path& root_can, const std::filesystem::path& candidate_norm) {
  auto ri = root_can.begin(), re = root_can.end();
  auto ti = candidate_norm.begin(), te = candidate_norm.end();
  for (; ri != re; ++ri, ++ti) {
    if (ti == te) return false;
    if (*ri != *ti) return false;
  }
  return true;
}

bool workspace_computed_target(const std::filesystem::path& root_can, const std::string& rel_utf8,
                               std::filesystem::path& out_target, std::string& err) {
  if (rel_utf8.empty()) {
    out_target = root_can;
    return true;
  }
  /** API 相对路径为 UTF-8（Windows 勿用 path(std::string)） */
  std::filesystem::path rel = std::filesystem::u8path(rel_utf8);
  if (!safe_rel_path(rel)) {
    err = "路径含非法分量";
    return false;
  }
  out_target = (root_can / rel).lexically_normal();
  if (!path_is_under_root(root_can, out_target)) {
    err = "路径越出工作区";
    return false;
  }
  return true;
}

static void trim_json_ws(std::string& x) {
  while (!x.empty() && static_cast<unsigned char>(x.front()) <= 32u) x.erase(x.begin());
  while (!x.empty() && static_cast<unsigned char>(x.back()) <= 32u) x.pop_back();
}

/** 将 workspaceRoot 注入客户端 JSON（客户端勿自行传绝对根路径） */
static std::string merge_workspace_json(const std::string& root_u8, const std::string& client_body) {
  std::string s = client_body;
  trim_json_ws(s);
  if (s.empty()) s = "{}";
  std::string inner;
  if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
    inner = s.substr(1, s.size() - 2);
    trim_json_ws(inner);
  }
  const std::string esc = cct::util::json_escape_string(root_u8);
  if (inner.empty()) return std::string("{\"workspaceRoot\":\"") + esc + "\"}";
  return std::string("{\"workspaceRoot\":\"") + esc + "\"," + inner + "}";
}

/** JSON body 中的 repo_rel：相对服务端 workspace_root 的子目录，作为 Git 仓库根（须已存在且为目录） */
static bool resolve_git_repo_abs_from_body(const std::filesystem::path& workspace_root_can,
                                           const std::string& req_body, std::filesystem::path& out_git_repo_can,
                                           std::string& err) {
  std::string repo_rel;
  if (!cct::util::json_extract_string_after_key(req_body, "repo_rel", repo_rel)) repo_rel.clear();
  trim_json_ws(repo_rel);
  std::replace(repo_rel.begin(), repo_rel.end(), '\\', '/');
  while (!repo_rel.empty() && repo_rel.front() == '/') repo_rel.erase(repo_rel.begin());
  while (!repo_rel.empty() && repo_rel.back() == '/') repo_rel.pop_back();

  if (repo_rel.empty()) {
    out_git_repo_can = workspace_root_can;
    return true;
  }
  std::filesystem::path tgt;
  if (!workspace_computed_target(workspace_root_can, repo_rel, tgt, err)) return false;
  std::error_code ec;
  if (!std::filesystem::is_directory(tgt, ec) || ec) {
    err = "repo_rel 不是已存在的目录";
    return false;
  }
  auto canon = std::filesystem::weakly_canonical(tgt, ec);
  if (ec) {
    err = "无法解析 repo_rel";
    return false;
  }
  if (!path_is_under_root(workspace_root_can, canon)) {
    err = "repo_rel 越出工作区";
    return false;
  }
  out_git_repo_can = canon;
  return true;
}

static bool workspace_resolve_existing_in_root(const std::filesystem::path& root_can, const std::string& rel_utf8,
                                               std::filesystem::path& out_canonical, std::string& err) {
  std::error_code ec;
  if (rel_utf8.empty()) {
    out_canonical = root_can;
    if (!std::filesystem::is_directory(out_canonical, ec) || ec) {
      err = "工作区根无效";
      return false;
    }
    return true;
  }
  std::filesystem::path target;
  if (!workspace_computed_target(root_can, rel_utf8, target, err)) return false;
  if (!std::filesystem::exists(target, ec) || ec) {
    err = "路径不存在";
    return false;
  }
  auto canon = std::filesystem::weakly_canonical(target, ec);
  if (ec) {
    err = "无法解析路径";
    return false;
  }
  if (!path_is_under_root(root_can, canon)) {
    err = "路径越界";
    return false;
  }
  out_canonical = canon;
  return true;
}

struct ScanSourceChunk {
  std::string rel_posix;
  std::string utf8_content;
};

static bool scan_skip_dir_name(const std::string& name) {
  return name == ".git" || name == "node_modules" || name == "__pycache__" || name == ".svn" ||
         name == "dist" || name == "build" || name == "target" || name == ".idea" || name == ".vscode" ||
         name == ".cache";
}

static bool scan_ext_ok(const std::string& ext_lc) {
  static const char* const k_ok[] = {
      ".cpp",  ".h",    ".hpp", ".cc",   ".cxx", ".c",    ".js",  ".mjs", ".ts",   ".tsx", ".jsx",
      ".vue",  ".py",   ".java", ".kt",   ".go",  ".rs",   ".swift", ".cs", ".php", ".rb", ".sql",
      ".md",   ".json", ".yaml", ".yml", ".toml", ".xml", ".css", ".scss", ".html", ".htm",
      ".sh",   ".bat",  ".ps1", ".cmake", ".txt"};
  for (auto* e : k_ok) {
    if (ext_lc == e) return true;
  }
  return false;
}

static std::string file_ext_lower(const std::filesystem::path& p) {
  auto e = p.extension().u8string();
  for (char& c : e) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return e;
}

static bool try_append_scan_text_file(const std::filesystem::path& abs_file, const std::string& rel_posix,
                                      std::vector<ScanSourceChunk>& out, std::size_t& total_so_far,
                                      std::size_t& file_count, std::string& err) {
  constexpr std::size_t k_scan_budget_total = 200000;
  constexpr std::size_t k_max_files = 120;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(abs_file, ec) || ec) return true;
  auto fsz = std::filesystem::file_size(abs_file, ec);
  if (ec || fsz > kWorkspaceMaxFile) {
    err = "单个文件过大或不可读: " + rel_posix;
    return false;
  }
  if (!scan_ext_ok(file_ext_lower(abs_file))) return true;
  if (file_count >= k_max_files) {
    err = "文件数量过多（单次上限 120 个），请缩小文件夹范围";
    return false;
  }
  std::ifstream fin(abs_file, std::ios::binary);
  if (!fin) {
    err = "无法读取: " + rel_posix;
    return false;
  }
  std::string body((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
  if (total_so_far + body.size() > k_scan_budget_total) {
    err = "选中内容超过单次检测上限（约 200KB 文本），请缩小范围";
    return false;
  }
  total_so_far += body.size();
  out.push_back(ScanSourceChunk{rel_posix, std::move(body)});
  ++file_count;
  return true;
}

static bool walk_dir_collect_scan(const std::filesystem::path& abs_dir, const std::string& rel_prefix,
                                  std::vector<ScanSourceChunk>& out, std::size_t& total_so_far,
                                  std::size_t& file_count, std::string& err) {
  std::error_code ec;
  for (const auto& de : std::filesystem::directory_iterator(abs_dir, ec)) {
    if (ec) break;
    std::string name = de.path().filename().u8string();
    if (name == "." || name == "..") continue;
    std::string sub_rel = rel_prefix.empty() ? name : (rel_prefix + "/" + name);
    if (de.is_directory(ec)) {
      if (scan_skip_dir_name(name)) continue;
      if (!walk_dir_collect_scan(de.path(), sub_rel, out, total_so_far, file_count, err)) return false;
      continue;
    }
    if (!try_append_scan_text_file(de.path(), sub_rel, out, total_so_far, file_count, err)) return false;
  }
  return true;
}

static bool collect_scan_chunks_for_paths(const std::filesystem::path& root_can,
                                          const std::vector<std::string>& paths_rel,
                                          std::vector<ScanSourceChunk>& out, std::string& err) {
  out.clear();
  std::size_t total = 0;
  std::size_t file_count = 0;
  for (const auto& pr : paths_rel) {
    std::filesystem::path tgt;
    if (!workspace_computed_target(root_can, pr, tgt, err)) return false;
    std::error_code ec;
    if (!std::filesystem::exists(tgt, ec) || ec) {
      err = "路径不存在: " + pr;
      return false;
    }
    std::string rel_norm = pr;
    std::replace(rel_norm.begin(), rel_norm.end(), '\\', '/');
    while (!rel_norm.empty() && rel_norm.back() == '/') rel_norm.pop_back();

    if (std::filesystem::is_regular_file(tgt, ec)) {
      if (!try_append_scan_text_file(tgt, rel_norm, out, total, file_count, err)) return false;
      continue;
    }
    if (std::filesystem::is_directory(tgt, ec)) {
      if (!walk_dir_collect_scan(tgt, rel_norm, out, total, file_count, err)) return false;
      continue;
    }
    err = "不支持的路径类型: " + pr;
    return false;
  }
  if (out.empty()) {
    err = "未找到可检测的文本源文件（请确认扩展名在白名单内，且文件夹非空）";
    return false;
  }
  return true;
}

static std::string extract_json_fence(const std::string& reply) {
  const std::string needle = "```json";
  size_t p = reply.find(needle);
  if (p == std::string::npos) return {};
  p += needle.size();
  while (p < reply.size() && (reply[p] == ' ' || reply[p] == '\r' || reply[p] == '\n' || reply[p] == '\t')) ++p;
  size_t q = reply.find("```", p);
  if (q == std::string::npos) return {};
  std::string raw = reply.substr(p, q - p);
  while (!raw.empty() && (raw.front() == '\r' || raw.front() == '\n')) raw.erase(raw.begin());
  while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
  return raw;
}

/** 提取 ```json 围栏：若无闭合 ```（常见于输出被 max_tokens 截断），则取至文末并标记 truncated */
static std::string extract_json_fence_relaxed(const std::string& reply, bool* truncated_out) {
  const std::string needle = "```json";
  size_t p = reply.find(needle);
  if (p == std::string::npos) {
    if (truncated_out) *truncated_out = false;
    return {};
  }
  p += needle.size();
  while (p < reply.size() && (reply[p] == ' ' || reply[p] == '\r' || reply[p] == '\n' || reply[p] == '\t')) ++p;
  size_t q = reply.find("```", p);
  const bool unclosed = (q == std::string::npos);
  if (truncated_out) *truncated_out = unclosed;
  size_t end = unclosed ? reply.size() : q;
  std::string raw = reply.substr(p, end - p);
  while (!raw.empty() && (raw.front() == '\r' || raw.front() == '\n')) raw.erase(raw.begin());
  while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
  return raw;
}

std::string mime_for(const std::filesystem::path& p) {
  auto ext = p.extension().string();
  if (ext == ".html") return "text/html; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".js") return "application/javascript; charset=utf-8";
  if (ext == ".json") return "application/json; charset=utf-8";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  return "application/octet-stream";
}

void handle_static(SOCKET s, const std::filesystem::path& ui_root, const std::string& url_path) {
  std::string p = url_path.empty() || url_path[0] != '/' ? std::string("/") + url_path : url_path;
  if (p == "/") p = "/index.html";
  std::filesystem::path rel = std::filesystem::path(p.substr(1));
  if (!safe_rel_path(rel)) {
    http::respond_text(s, 404, "text/plain; charset=utf-8", "not found");
    return;
  }
  std::filesystem::path full = ui_root / rel;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(full, ec)) {
    http::respond_text(s, 404, "text/plain; charset=utf-8", "not found");
    return;
  }
  std::ifstream in(full, std::ios::binary);
  if (!in) {
    http::respond_text(s, 404, "text/plain; charset=utf-8", "not found");
    return;
  }
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  http::respond_text(s, 200, mime_for(full), body);
}

Session* session_from_req(const http::HttpReq& req) {
  std::string tok = http::cookie_value(req, "cct_session");
  if (tok.empty()) return nullptr;
  std::lock_guard<std::mutex> lock(g_sess_mu);
  auto it = g_sessions.find(tok);
  if (it == g_sessions.end()) return nullptr;
  return &it->second;
}
void handle_api(SOCKET s, http::HttpReq& req, cct::storage::IUserPersistence& users,
                cct::storage::IChatPersistence& chats, cct::storage::IComponentPersistence& comps,
                cct::storage::ITokenBillingPersistence& billing, const std::filesystem::path& data_dir,
                const cct::util::AppConfig& llm_cfg) {
  const std::string& path = req.path;

  /** 不含密钥：便于前端展示「当前用哪个模型」 */
  if (req.method == "GET" && path == "/api/llm-info") {
    std::string prov;
    for (char c : llm_cfg.llm_provider) prov += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    std::string prov_out = llm_cfg.use_mock ? std::string("mock") : prov;
    std::string models_json = "[]";
    if (prov_out == "zhipu")
      models_json = "[\"glm-4.7-flash\",\"glm-4\",\"glm-4-flash\",\"glm-4-air\",\"glm-z1-flash\"]";
    else if (prov_out == "anthropic")
      models_json = "[\"claude-sonnet-4-20250514\",\"claude-3-5-sonnet-20241022\"]";
    http::respond_json(s, 200,
                       json_ok(",\"provider\":\"" + cct::util::json_escape_string(prov_out) + "\",\"model\":\"" +
                               cct::util::json_escape_string(llm_cfg.model) + "\",\"use_mock\":" +
                               (llm_cfg.use_mock ? "true" : "false") + ",\"api_host\":\"" +
                               cct::util::json_escape_string(llm_cfg.api_host) + "\",\"models\":" + models_json));
    return;
  }

  if (req.method == "GET" && path == "/api/captcha") {
    std::vector<unsigned char> rb;
    if (!cct::web::crypto::random_bytes(rb, 3)) {
      http::respond_json(s, 500, json_err("随机数失败"));
      return;
    }
    const unsigned n = (static_cast<unsigned>(rb[0]) << 16) | (static_cast<unsigned>(rb[1]) << 8) | rb[2];
    char digits[8];
    std::snprintf(digits, sizeof(digits), "%04u", n % 10000);
    std::string ans = digits;
    std::string cid = new_session_token();
    if (cid.empty()) {
      http::respond_json(s, 500, json_err("会话令牌失败"));
      return;
    }
    {
      std::lock_guard<std::mutex> lk(g_captcha_mu);
      captcha_prune_locked();
      g_captchas[cid] = CaptchaEntry{ans, steady_now_ms() + 300000};
    }
    const std::string svg = svg_captcha_4digits(ans);
    http::respond_json(s, 200,
                       json_ok(",\"id\":\"" + cct::util::json_escape_string(cid) + "\",\"svg\":\"" +
                               cct::util::json_escape_string(svg) + "\""));
    return;
  }

  if (req.method == "POST" && path == "/api/register") {
    std::string user, pass, captcha_id, captcha_answer;
    if (!cct::util::json_extract_string_after_key(req.body, "username", user) ||
        !cct::util::json_extract_string_after_key(req.body, "password", pass)) {
      http::respond_json(s, 400, json_err("需要 username 与 password"));
      return;
    }
    if (!cct::util::json_extract_string_after_key(req.body, "captcha_id", captcha_id) ||
        !cct::util::json_extract_string_after_key(req.body, "captcha_answer", captcha_answer)) {
      http::respond_json(s, 400, json_err("需要验证码 captcha_id 与 captcha_answer"));
      return;
    }
    while (!captcha_answer.empty() && (captcha_answer.front() == ' ' || captcha_answer.front() == '\t'))
      captcha_answer.erase(captcha_answer.begin());
    while (!captcha_answer.empty() && (captcha_answer.back() == ' ' || captcha_answer.back() == '\t'))
      captcha_answer.pop_back();
    bool captcha_ok = false;
    {
      std::lock_guard<std::mutex> lk(g_captcha_mu);
      captcha_prune_locked();
      auto it = g_captchas.find(captcha_id);
      if (it != g_captchas.end() && it->second.answer == captcha_answer) {
        captcha_ok = true;
        g_captchas.erase(it);
      }
    }
    if (!captcha_ok) {
      http::respond_json(s, 400, json_err("验证码错误或已过期"));
      return;
    }
    if (user.size() < 2 || user.size() > 32) {
      http::respond_json(s, 400, json_err("用户名长度 2–32"));
      return;
    }
    if (pass.size() < 4) {
      http::respond_json(s, 400, json_err("密码至少 4 位"));
      return;
    }
    std::string err;
    std::uint64_t uid = 0;
    if (!users.register_user(user, pass, uid, err)) {
      http::respond_json(s, 400, json_err(err));
      return;
    }
    http::respond_json(s, 200, json_ok(",\"id\":\"" + std::to_string(uid) + "\""));
    return;
  }

  if (req.method == "POST" && path == "/api/login") {
    std::string user, pass;
    if (!cct::util::json_extract_string_after_key(req.body, "username", user) ||
        !cct::util::json_extract_string_after_key(req.body, "password", pass)) {
      http::respond_json(s, 400, json_err("需要 username 与 password"));
      return;
    }
    std::string err;
    std::uint64_t uid = 0;
    if (!users.verify_login(user, pass, uid, err)) {
      http::respond_json(s, 401, json_err(err));
      return;
    }
    std::string tok = new_session_token();
    if (tok.empty()) {
      http::respond_json(s, 500, json_err("会话令牌失败"));
      return;
    }
    {
      std::lock_guard<std::mutex> sl(g_sess_mu);
      g_sessions[tok] = Session{uid, user};
    }
    std::string disp = user;
    cct::storage::UserRowLite ur;
    std::string ge;
    if (users.get_user_by_id(uid, ur, ge) && !ur.display_name.empty()) disp = ur.display_name;
    std::string cookie =
        "Set-Cookie: cct_session=" + tok + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=8640000\r\n";
    http::respond_json(s, 200,
                       json_ok(",\"username\":\"" + cct::util::json_escape_string(user) + "\",\"displayName\":\"" +
                               cct::util::json_escape_string(disp) + "\""),
                       cookie);
    return;
  }

  if (req.method == "POST" && path == "/api/logout") {
    std::string tok = http::cookie_value(req, "cct_session");
    if (!tok.empty()) {
      std::lock_guard<std::mutex> sl(g_sess_mu);
      g_sessions.erase(tok);
    }
    std::string cookie = "Set-Cookie: cct_session=; Path=/; HttpOnly; Max-Age=0\r\n";
    http::respond_json(s, 200, json_ok(""), cookie);
    return;
  }

  if (req.method == "GET" && path == "/api/me") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    cct::storage::UserRowLite ur;
    std::string ge;
    std::string disp = se->username;
    if (users.get_user_by_id(se->user_id, ur, ge) && !ur.display_name.empty()) disp = ur.display_name;
    const LlmUsageSnap lu = peek_llm_usage_for_me(data_dir, se->user_id);
    cct::storage::TokenBillingState bs{};
    std::string berr;
    if (!billing.get_state(se->user_id, bs, berr)) {
      http::respond_json(s, 500, json_err(berr.empty() ? "计费状态读取失败" : berr));
      return;
    }
    const std::string tier_n = cct::storage::billing_normalize_tier(bs.tier);
    const std::int64_t rem = bs.token_quota - bs.tokens_consumed;
    const std::string sub_lbl = cct::storage::billing_subscription_label_zh(tier_n);
    const int eff_daily = cct::storage::billing_effective_daily_llm_calls(tier_n, llm_cfg.llm_daily_call_limit);
    std::string uit = "dark";
    std::string uie;
    (void)users.get_ui_theme(se->user_id, uit, uie);
    http::respond_json(s, 200,
                       json_ok(",\"userId\":\"" + std::to_string(se->user_id) + "\",\"username\":\"" +
                               cct::util::json_escape_string(se->username) + "\",\"displayName\":\"" +
                               cct::util::json_escape_string(disp) + "\",\"llmDailyLimit\":" +
                               std::to_string(eff_daily) + ",\"llmCallsToday\":" +
                               std::to_string(lu.calls_today) + ",\"llmTotalCalls\":" + std::to_string(lu.total_calls) +
                               ",\"subscriptionTier\":\"" + cct::util::json_escape_string(tier_n) +
                               "\",\"subscriptionLabel\":\"" + cct::util::json_escape_string(sub_lbl) +
                               "\",\"tokenQuota\":" + std::to_string(bs.token_quota) + ",\"tokensConsumed\":" +
                               std::to_string(bs.tokens_consumed) + ",\"tokensRemaining\":" + std::to_string(rem) +
                               ",\"periodYm\":" + std::to_string(bs.period_yyyymm) + ",\"uiTheme\":\"" +
                               cct::util::json_escape_string(uit) + "\""));
    return;
  }

  if (req.method == "GET" && path == "/api/analytics/summary") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::size_t threads_storage_count = 0;
    const std::string uk_an = cct::storage::user_chats_key(se->user_id);
    {
      std::lock_guard<std::mutex> cl(chats.mutex());
      chats.ensure_loaded(se->user_id);
      threads_storage_count = chats.threads_map()[uk_an].size();
    }
    const LlmUsageSnap lu = peek_llm_usage_for_me(data_dir, se->user_id);
    cct::storage::TokenBillingState bs_an{};
    std::string ban_err;
    if (!billing.get_state(se->user_id, bs_an, ban_err)) {
      http::respond_json(s, 500, json_err(ban_err.empty() ? "计费状态读取失败" : ban_err));
      return;
    }
    const std::string tier_an = cct::storage::billing_normalize_tier(bs_an.tier);
    const int eff_lim_an =
        cct::storage::billing_effective_daily_llm_calls(tier_an, llm_cfg.llm_daily_call_limit);
    const std::string inner = analytics_build_summary_inner(
        data_dir, se->user_id, lu, eff_lim_an, threads_storage_count, tier_an, bs_an.token_quota,
        bs_an.tokens_consumed);
    http::respond_json(s, 200, json_ok(inner));
    return;
  }

  if (req.method == "PUT" && path == "/api/me/profile") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string display_name;
    if (!cct::util::json_extract_string_after_key(req.body, "display_name", display_name)) {
      http::respond_json(s, 400, json_err("需要 display_name"));
      return;
    }
    std::string err;
    if (!users.update_display_name(se->user_id, display_name, err)) {
      http::respond_json(s, 400, json_err(err));
      return;
    }
    cct::storage::UserRowLite ur;
    std::string ge;
    std::string disp = se->username;
    if (users.get_user_by_id(se->user_id, ur, ge) && !ur.display_name.empty()) disp = ur.display_name;
    http::respond_json(s, 200,
                       json_ok(",\"displayName\":\"" + cct::util::json_escape_string(disp) + "\",\"username\":\"" +
                               cct::util::json_escape_string(se->username) + "\""));
    return;
  }

  if (req.method == "PUT" && path == "/api/me/preferences") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string theme_raw;
    if (!cct::util::json_extract_string_after_key(req.body, "theme", theme_raw)) {
      http::respond_json(s, 400, json_err("需要 theme"));
      return;
    }
    std::string err_pf;
    if (!users.set_ui_theme(se->user_id, theme_raw, err_pf)) {
      http::respond_json(s, 400, json_err(err_pf));
      return;
    }
    std::string uit = "dark";
    (void)users.get_ui_theme(se->user_id, uit, err_pf);
    http::respond_json(s, 200,
                       json_ok(",\"theme\":\"" + cct::util::json_escape_string(uit) + "\""));
    return;
  }

  if (req.method == "GET" && path == "/api/billing/plans") {
    http::respond_json(s, 200, json_ok(billing_plans_json_inner()));
    return;
  }

  if (req.method == "POST" && path == "/api/billing/subscribe") {
    Session* bse = session_from_req(req);
    if (!bse) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string tier_raw, pay_method;
    if (!cct::util::json_extract_string_after_key(req.body, "tier", tier_raw) ||
        !cct::util::json_extract_string_after_key(req.body, "payMethod", pay_method)) {
      http::respond_json(s, 400, json_err("需要 tier 与 payMethod"));
      return;
    }
    std::string sub_err, txn;
    if (!billing.apply_subscription(bse->user_id, tier_raw, pay_method, txn, sub_err)) {
      http::respond_json(s, 400, json_err(sub_err));
      return;
    }
    cct::storage::TokenBillingState bs2{};
    if (!billing.get_state(bse->user_id, bs2, sub_err)) {
      http::respond_json(s, 200, json_ok(",\"transactionId\":\"" + cct::util::json_escape_string(txn) + "\""));
      return;
    }
    const std::string tier_n2 = cct::storage::billing_normalize_tier(bs2.tier);
    const std::int64_t rem2 = bs2.token_quota - bs2.tokens_consumed;
    http::respond_json(s, 200,
                       json_ok(",\"transactionId\":\"" + cct::util::json_escape_string(txn) +
                               "\",\"subscriptionTier\":\"" + cct::util::json_escape_string(tier_n2) +
                               "\",\"tokenQuota\":" + std::to_string(bs2.token_quota) +
                               ",\"tokensConsumed\":" + std::to_string(bs2.tokens_consumed) +
                               ",\"tokensRemaining\":" + std::to_string(rem2) +
                               ",\"periodYm\":" + std::to_string(bs2.period_yyyymm)));
    return;
  }

  if (req.method == "POST" && path == "/api/code-scan/run") {
    Session* scan_se = session_from_req(req);
    if (!scan_se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::vector<std::string> paths_raw;
    std::vector<std::string> dims_raw;
    if (!extract_json_string_array_simple(req.body, "paths", paths_raw) || paths_raw.empty()) {
      http::respond_json(s, 400, json_err("需要 paths 非空数组"));
      return;
    }
    if (!extract_json_string_array_simple(req.body, "dimensions", dims_raw) || dims_raw.empty()) {
      http::respond_json(s, 400, json_err("需要 dimensions 非空数组"));
      return;
    }
    std::vector<std::string> dims_norm;
    for (const auto& raw : dims_raw) {
      std::string d = raw;
      for (char& c : d) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (d != "performance" && d != "maintainability" && d != "robustness" && d != "completeness" &&
          d != "security")
        continue;
      bool dup = false;
      for (const auto& e : dims_norm)
        if (e == d) dup = true;
      if (!dup) dims_norm.push_back(d);
    }
    if (dims_norm.empty()) {
      http::respond_json(s, 400,
                         json_err("dimensions 需包含有效维度：performance / maintainability / robustness / "
                                  "completeness / security"));
      return;
    }
    std::filesystem::path scan_root;
    std::string scan_wroot_err;
    if (!resolve_workspace_root_canonical(llm_cfg, scan_root, scan_wroot_err)) {
      http::respond_json(s, 503, json_err(scan_wroot_err));
      return;
    }
    std::vector<ScanSourceChunk> scan_chunks;
    std::string coll_err;
    if (!collect_scan_chunks_for_paths(scan_root, paths_raw, scan_chunks, coll_err)) {
      http::respond_json(s, 400, json_err(coll_err));
      return;
    }
    std::ostringstream scan_um;
    scan_um << "你是资深代码审计助手。请严格依据下列「维度规则」审阅代码，输出中文结论。\n\n";
    scan_um << "## 维度规则\n" << code_scan_rules_for_dimensions(dims_norm);
    scan_um << "\n## 待审代码\n";
    scan_um << "说明：下列 FILE 列表覆盖用户所选路径；若为文件夹则已递归读取其中允许的源码文件（单次有总字数与文件数上限）。若多个文件存在头文件/实现或其它耦合，请在审计结论与 patches 中联动修改，保持可编译与语义一致。\n";
    for (const auto& ch : scan_chunks) {
      scan_um << "\n### FILE " << ch.rel_posix << "\n```\n" << ch.utf8_content << "\n```\n";
    }
    scan_um << "\n## 输出要求（严格遵守）\n"
               "1) 先用 **Markdown** 写简短「审计摘要」（可分小节）。**Markdown 正文只讨论下列已选维度**，勿引入用户未勾选的维度。\n"
               "2) **必须**针对用户所选维度逐一写明小结（每个已选维度至少一条具体问题或可点赞）；维度 ID 只能使用英文标识 performance/maintainability/robustness/completeness/security。\n"
               "3) 随后给出「解决方案概要」（中文段落）：说明你将对代码做哪些修改及原因。\n"
               "4) **最后一个围栏**：单独追加 ```json 代码块（仅此一处），内容必须为合法 JSON，且可被解析，字段如下：\n"
               "- summary：字符串，全文一句话概述。\n"
               "- dimension_results：数组；每项含 dimension（**必须为第 5 条所列 ID 之一**）、title（中文维度名）、items（字符串数组，该维度下的具体问题或结论）、severity（可选字符串 low/medium/high）。\n"
               "- solution_summary：字符串，与上文解决方案概要一致或提炼。\n"
               "- patches：数组；每项 path 为工作区相对路径（正斜杠）、content 为该文件**完整替换**后的 UTF-8 正文。\n"
               "若无代码改动则 patches 为 []；若仅需删除逻辑可把无关代码删掉并保持文件语法合法。patches 中 path 必须对应上文「FILE」路径。\n";
    scan_um << "5) dimension_results 中每条记录的 dimension **必须是下列之一且仅限下列取值**，禁止包含用户未勾选的维度：";
    for (size_t i = 0; i < dims_norm.size(); ++i) {
      if (i) scan_um << "、";
      scan_um << dims_norm[i];
    }
    scan_um << "。\n"
               "6) JSON 围栏务必精简：若本轮无需改文件则 patches 必须为 []；dimension_results 每条 items 不超过 4 条简短要点；勿在 JSON 中重复粘贴 FILE 正文。\n";
    const std::string scan_user_blob = scan_um.str();
    std::vector<cct::llm::ChatMessage> scan_msgs;
    scan_msgs.push_back({"user", scan_user_blob});
    cct::util::AppConfig cfg_scan = llm_cfg;
    /** 代码检测单次输出含 Markdown + JSON；下限抬高以减少截断（仍取 max(配置, 下限)，可自行把配置 max_tokens 调更大） */
    constexpr int k_code_scan_min_max_tokens = 16384;
    cfg_scan.max_tokens = (std::max)(llm_cfg.max_tokens, k_code_scan_min_max_tokens);
    std::string scan_model_pick;
    if (cct::util::json_extract_string_after_key(req.body, "model", scan_model_pick) && !scan_model_pick.empty()) {
      std::string prov_lc;
      for (char c : llm_cfg.llm_provider) prov_lc += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (allowed_chat_model_for_provider(prov_lc, scan_model_pick)) cfg_scan.model = scan_model_pick;
    }
    constexpr std::int64_t k_billing_min_scan = 2048;
    cct::storage::TokenBillingState scan_bill{};
    std::string scan_bill_err;
    const std::int64_t scan_pre =
        (std::max)(k_billing_min_scan, static_cast<std::int64_t>(scan_user_blob.size() / 8));
    if (!billing.check_can_use(scan_se->user_id, scan_pre, scan_bill, scan_bill_err)) {
      http::respond_json(s, 429, json_err_msg_code(scan_bill_err, "token_exhausted"));
      return;
    }
    const std::string scan_tier_n = cct::storage::billing_normalize_tier(scan_bill.tier);
    std::string scan_daily_den;
    if (tier_daily_call_blocked(data_dir, scan_tier_n, scan_se->user_id, llm_cfg.llm_daily_call_limit, scan_daily_den)) {
      http::respond_json(s, 429, json_err_msg_code(scan_daily_den, "daily_limit_exceeded"));
      return;
    }
    const std::uint64_t scan_llm_t0 = steady_now_ms();
    cct::llm::LlmResult scan_out;
    if (cfg_scan.use_mock) {
      scan_out.ok = true;
      std::ostringstream mock_md;
      mock_md << "### Mock\n未连接真实模型时的占位审计。\n";
      std::ostringstream mock_fence;
      mock_fence << "{\"summary\":\"Mock\",\"dimension_results\":[";
      bool first_dim = true;
      for (const auto& d : dims_norm) {
        mock_md << "#### " << d << "\n- （占位）请启用真实模型。\n\n";
        if (!first_dim) mock_fence << ",";
        first_dim = false;
        const std::string tit = code_scan_dim_title_cn(d);
        mock_fence << "{\"dimension\":\"" << d << "\",\"title\":\"" << cct::util::json_escape_string(tit)
                   << "\",\"items\":[\"占位\"],\"severity\":\"low\"}";
      }
      mock_fence << "],\"solution_summary\":\"连接真实模型以获得补丁\",\"patches\":[]}";
      scan_out.text =
          mock_md.str() + "\n```json\n" + mock_fence.str() + "\n```\n";
    } else {
      std::string scan_prov;
      for (char c : cfg_scan.llm_provider) scan_prov += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (scan_prov == "zhipu")
        scan_out = cct::llm::call_zhipu_chat(cfg_scan, scan_msgs);
      else
        scan_out = cct::llm::call_anthropic_chat(cfg_scan, scan_msgs);
    }
    const std::uint64_t scan_llm_ms = steady_now_ms() - scan_llm_t0;
    analytics_emit_scan_completion(data_dir, scan_se->user_id, static_cast<int>(paths_raw.size()),
                                   static_cast<int>(dims_norm.size()), cfg_scan.model, scan_out.ok, scan_llm_ms,
                                   scan_out.usage_prompt_tokens, scan_out.usage_completion_tokens,
                                   scan_out.usage_total_tokens);
    if (!scan_out.ok) {
      http::respond_json(s, 502, json_err(scan_out.error));
      return;
    }
    bool scan_fence_truncated = false;
    std::string scan_fence = extract_json_fence_relaxed(scan_out.text, &scan_fence_truncated);
    std::ostringstream scan_json_extra;
    scan_json_extra << ",\"reply\":\"" << cct::util::json_escape_string(scan_out.text) << "\"";
    if (!scan_fence.empty())
      scan_json_extra << ",\"structured_json\":\"" << cct::util::json_escape_string(scan_fence) << "\"";
    else
      scan_json_extra << ",\"structured_json\":null";
    scan_json_extra << ",\"scan_truncated\":" << (scan_fence_truncated ? "true" : "false");
    {
      std::size_t apx = scan_user_blob.size();
      std::int64_t scan_delta = billing_delta_from_llm_usage(
          scan_out.usage_prompt_tokens, scan_out.usage_completion_tokens, k_billing_min_scan, apx);
      cct::storage::TokenBillingState st_scan{};
      std::string adde;
      (void)billing.add_consumed(scan_se->user_id, scan_delta, st_scan, adde);
    }
    record_llm_call_success_for_analytics(data_dir, scan_se->user_id);
    http::respond_json(s, 200, json_ok(scan_json_extra.str()));
    return;
  }

  if (req.path.rfind("/api/workspace/", 0) == 0) {
    Session* wse = session_from_req(req);
    if (!wse) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::error_code wsec;
    std::filesystem::path root_can;
    {
      std::string wroot_err;
      if (!resolve_workspace_root_canonical(llm_cfg, root_can, wroot_err)) {
        http::respond_json(s, 503, json_err(wroot_err));
        return;
      }
    }

    /** Git：转发至本机 127.0.0.1:git_worker_port（simple-git） */
    if (req.method == "POST" && path.rfind("/api/workspace/git/", 0) == 0 && path.size() > 19) {
      std::string tail = path.substr(19);
      if (tail.empty()) {
        http::respond_json(s, 400, json_err("无效 git 子路径"));
        return;
      }
      const std::string worker_path = std::string("/git/") + tail;
      std::filesystem::path git_repo_can;
      std::string grepo_err;
      if (!resolve_git_repo_abs_from_body(root_can, req.body, git_repo_can, grepo_err)) {
        http::respond_json(s, 400, json_err(grepo_err));
        return;
      }
      std::string merged = merge_workspace_json(git_repo_can.u8string(), req.body);
      int pst = 0;
      std::string resp;
      std::string gerr;
      const bool long_op = (tail == "push" || tail == "pull");
      const int timeout_ms = long_op ? 300000 : 120000;
      if (!cct::web::git_worker_http_post(llm_cfg.git_worker_port, worker_path, merged, timeout_ms, pst, resp,
                                          gerr)) {
        http::respond_json(s, 503, json_err(gerr.empty() ? "git-worker 不可用" : gerr));
        return;
      }
      if (pst <= 0) pst = 502;
      http::respond_json(s, pst, resp);
      return;
    }

    if (req.method == "GET" && path == "/api/workspace/status") {
      http::respond_json(s, 200, json_ok(",\"enabled\":true,\"root\":\"" +
                                            cct::util::json_escape_string(root_can.u8string()) + "\""));
      return;
    }

    if (req.method == "GET" && path == "/api/workspace/list") {
      std::string relq;
      query_string_param(req.query, "path", relq);
      std::filesystem::path dirp;
      std::string werr;
      if (!workspace_resolve_existing_in_root(root_can, relq, dirp, werr)) {
        http::respond_json(s, 400, json_err(werr));
        return;
      }
      if (!std::filesystem::is_directory(dirp)) {
        http::respond_json(s, 400, json_err("不是目录"));
        return;
      }
      std::ostringstream arr;
      arr << ",\"entries\":[";
      bool first = true;
      size_t n = 0;
      for (const auto& de : std::filesystem::directory_iterator(dirp, wsec)) {
        if (wsec) break;
        if (n++ >= 500) break;
        std::string name = de.path().filename().u8string();
        if (name == "." || name == "..") continue;
        if (!first) arr << ",";
        first = false;
        bool is_dir = de.is_directory(wsec);
        arr << "{\"name\":\"" << cct::util::json_escape_string(name) << "\",\"type\":\"" << (is_dir ? "dir" : "file")
            << "\"}";
      }
      arr << "]";
      http::respond_json(s, 200, json_ok(arr.str()));
      return;
    }

    if (req.method == "GET" && path == "/api/workspace/file") {
      std::string relq;
      if (!query_string_param(req.query, "path", relq) || relq.empty()) {
        http::respond_json(s, 400, json_err("需要 query 参数 path"));
        return;
      }
      std::filesystem::path fp;
      std::string werr;
      if (!workspace_resolve_existing_in_root(root_can, relq, fp, werr)) {
        http::respond_json(s, 400, json_err(werr));
        return;
      }
      if (!std::filesystem::is_regular_file(fp)) {
        http::respond_json(s, 400, json_err("不是普通文件"));
        return;
      }
      auto fsz = std::filesystem::file_size(fp, wsec);
      if (wsec || fsz > kWorkspaceMaxFile) {
        http::respond_json(s, 413, json_err("文件过大（上限 2MB）"));
        return;
      }
      std::ifstream fin(fp, std::ios::binary);
      std::string fbody((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
      http::respond_json(s, 200,
                         json_ok(",\"path\":\"" + cct::util::json_escape_string(relq) + "\",\"content\":\"" +
                                 cct::util::json_escape_string(fbody) + "\""));
      return;
    }

    if ((req.method == "PUT" && path == "/api/workspace/file") ||
        (req.method == "POST" && (path == "/api/workspace/file" || path == "/api/workspace/apply"))) {
      std::string relw, contentw;
      if (!cct::util::json_extract_string_after_key(req.body, "path", relw) || relw.empty()) {
        http::respond_json(s, 400, json_err("需要 JSON 字段 path（非空）"));
        return;
      }
      if (!cct::util::json_extract_string_after_key(req.body, "content", contentw)) contentw.clear();
      std::filesystem::path tgt;
      std::string werr2;
      if (!workspace_computed_target(root_can, relw, tgt, werr2)) {
        http::respond_json(s, 400, json_err(werr2));
        return;
      }
      std::filesystem::path par = tgt.parent_path().lexically_normal();
      if (!path_is_under_root(root_can, par)) {
        http::respond_json(s, 403, json_err("父目录越界"));
        return;
      }
      if (contentw.size() > kWorkspaceMaxFile) {
        http::respond_json(s, 413, json_err("内容过大（上限 2MB）"));
        return;
      }
      std::filesystem::create_directories(par, wsec);
      std::ofstream ofs(tgt, std::ios::binary | std::ios::trunc);
      if (!ofs) {
        http::respond_json(s, 500, json_err("写入失败"));
        return;
      }
      ofs << contentw;
      std::string ws_op = "post";
      if (req.method == "PUT")
        ws_op = "put";
      else if (path == "/api/workspace/apply")
        ws_op = "apply";
      analytics_emit_ws_event(data_dir, wse->user_id, ws_op, relw, static_cast<int>(contentw.size()));
      http::respond_json(s, 200, json_ok(""));
      return;
    }

    if (req.method == "POST" && path == "/api/workspace/mkdir") {
      std::string relm;
      if (!cct::util::json_extract_string_after_key(req.body, "path", relm) || relm.empty()) {
        http::respond_json(s, 400, json_err("需要 path"));
        return;
      }
      std::filesystem::path tgt;
      std::string wmk_err;
      if (!workspace_computed_target(root_can, relm, tgt, wmk_err)) {
        http::respond_json(s, 400, json_err(wmk_err));
        return;
      }
      std::error_code mk_ec;
      std::filesystem::create_directories(tgt, mk_ec);
      if (mk_ec) {
        http::respond_json(s, 500, json_err(std::string("创建目录失败: ") + mk_ec.message()));
        return;
      }
      analytics_emit_ws_event(data_dir, wse->user_id, "mkdir", relm, 0);
      http::respond_json(s, 200, json_ok(""));
      return;
    }

    if (req.method == "DELETE" && path == "/api/workspace/file") {
      std::string relq;
      if (!query_string_param(req.query, "path", relq) || relq.empty()) {
        http::respond_json(s, 400, json_err("需要 query 参数 path"));
        return;
      }
      std::filesystem::path fp;
      std::string werr;
      if (!workspace_resolve_existing_in_root(root_can, relq, fp, werr)) {
        http::respond_json(s, 400, json_err(werr));
        return;
      }
      std::error_code ec3;
      const bool is_file = std::filesystem::is_regular_file(fp, ec3);
      const bool is_dir = !ec3 && std::filesystem::is_directory(fp, ec3);
      if (!is_file && !is_dir) {
        http::respond_json(s, 400, json_err("路径不存在或既不是文件也不是目录"));
        return;
      }
      if (is_file) {
        if (!std::filesystem::remove(fp, ec3) || ec3) {
          http::respond_json(s, 500, json_err(std::string("删除文件失败: ") + ec3.message()));
          return;
        }
      } else {
        /** 递归删除目录（毕设演示）；请勿对工作区根 path="" 调用 */
        if (relq.empty()) {
          http::respond_json(s, 400, json_err("不能删除工作区根目录"));
          return;
        }
        const auto removed = std::filesystem::remove_all(fp, ec3);
        if (ec3) {
          http::respond_json(s, 500, json_err(std::string("删除目录失败: ") + ec3.message()));
          return;
        }
        (void)removed;
      }
      http::respond_json(s, 200, json_ok(""));
      return;
    }

    http::respond_json(s, 404, json_err("未知 workspace 接口"));
    return;
  }

  if (req.method == "GET" && path == "/api/chat/threads") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    const std::string uk = cct::storage::user_chats_key(se->user_id);
    std::lock_guard<std::mutex> cl(chats.mutex());
    chats.ensure_loaded(se->user_id);
    cct::storage::ensure_default_thread_list_vec(chats.threads_map()[uk]);
    std::string q_raw;
    query_string_param(req.query, "q", q_raw);
    const std::string q = uri_percent_decode_utf8(q_raw);
    std::ostringstream arr;
    arr << ",\"threads\":[";
    bool first = true;
    for (const auto& row : chats.threads_map()[uk]) {
      if (!q.empty() && row.title.find(q) == std::string::npos) continue;
      if (!first) arr << ',';
      first = false;
      arr << "{\"id\":\"" << cct::util::json_escape_string(row.id) << "\",\"title\":\""
          << cct::util::json_escape_string(row.title) << "\",\"workspaceAnchor\":\""
          << cct::util::json_escape_string(row.workspace_anchor) << "\"}";
    }
    arr << "]";
    http::respond_json(s, 200, json_ok(arr.str()));
    return;
  }

  if (req.method == "GET" && path == "/api/chat/thread") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string tid;
    if (!query_string_param(req.query, "id", tid) || tid.empty()) {
      http::respond_json(s, 400, json_err("需要 query 参数 id"));
      return;
    }
    if (!valid_thread_id_str(tid)) {
      http::respond_json(s, 400, json_err("非法对话 id"));
      return;
    }
    const std::string uk = cct::storage::user_chats_key(se->user_id);
    std::lock_guard<std::mutex> cl(chats.mutex());
    chats.ensure_loaded(se->user_id);
    const std::string key = cct::storage::chat_hist_key(uk, tid);
    std::vector<cct::llm::ChatMessage> mh;
    auto it = chats.history_map().find(key);
    if (it != chats.history_map().end()) mh = it->second;
    std::string anchor;
    for (const auto& row : chats.threads_map()[uk]) {
      if (row.id == tid) {
        anchor = row.workspace_anchor;
        break;
      }
    }
    http::respond_json(s, 200,
                       json_ok(",\"threadId\":\"" + cct::util::json_escape_string(tid) +
                               "\",\"workspaceAnchor\":\"" + cct::util::json_escape_string(anchor) +
                               "\",\"messages\":" + messages_to_json_array(mh)));
    return;
  }

  if (req.method == "DELETE" && path == "/api/chat/thread") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string tid;
    if (!query_string_param(req.query, "id", tid) || tid.empty()) {
      http::respond_json(s, 400, json_err("需要 query 参数 id"));
      return;
    }
    if (!valid_thread_id_str(tid)) {
      http::respond_json(s, 400, json_err("非法对话 id"));
      return;
    }
    if (tid == "main") {
      http::respond_json(s, 400, json_err("不能删除默认对话"));
      return;
    }
    const std::string uk = cct::storage::user_chats_key(se->user_id);
    std::lock_guard<std::mutex> cl(chats.mutex());
    chats.ensure_loaded(se->user_id);
    cct::storage::ensure_default_thread_list_vec(chats.threads_map()[uk]);
    auto& rows = chats.threads_map()[uk];
    auto row_it = std::find_if(rows.begin(), rows.end(),
                               [&](const cct::storage::ChatThreadRow& r) { return r.id == tid; });
    if (row_it == rows.end()) {
      http::respond_json(s, 404, json_err("对话不存在"));
      return;
    }
    const std::filesystem::path mj =
        data_dir / "users" / std::to_string(se->user_id) / "chats" / ("m_" + safe_thread_file_stem(tid) + ".json");
    std::error_code ec_rem;
    std::filesystem::remove(mj, ec_rem);
    rows.erase(row_it);
    chats.history_map().erase(cct::storage::chat_hist_key(uk, tid));
    chats.persist(se->user_id);
    http::respond_json(s, 200, json_ok(""));
    return;
  }

  if (req.method == "PUT" && path == "/api/chat/thread") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string tid;
    std::string title;
    if (!cct::util::json_extract_string_after_key(req.body, "thread_id", tid) || tid.empty()) {
      http::respond_json(s, 400, json_err("需要 thread_id"));
      return;
    }
    if (!cct::util::json_extract_string_after_key(req.body, "title", title)) {
      http::respond_json(s, 400, json_err("需要 title"));
      return;
    }
    if (!valid_thread_id_str(tid)) {
      http::respond_json(s, 400, json_err("非法对话 id"));
      return;
    }
    sanitize_thread_rename_title(title);
    if (title.empty()) {
      http::respond_json(s, 400, json_err("标题不能为空"));
      return;
    }
    const std::string uk = cct::storage::user_chats_key(se->user_id);
    std::lock_guard<std::mutex> cl(chats.mutex());
    chats.ensure_loaded(se->user_id);
    cct::storage::ensure_default_thread_list_vec(chats.threads_map()[uk]);
    bool found = false;
    for (auto& row : chats.threads_map()[uk]) {
      if (row.id != tid) continue;
      row.title = title;
      row.updated++;
      found = true;
      break;
    }
    if (!found) {
      http::respond_json(s, 404, json_err("对话不存在"));
      return;
    }
    chats.persist(se->user_id);
    http::respond_json(s, 200,
                       json_ok(",\"threadId\":\"" + cct::util::json_escape_string(tid) + "\",\"title\":\"" +
                               cct::util::json_escape_string(title) + "\""));
    return;
  }

  if (req.method == "POST" && path == "/api/chat/threads") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::vector<unsigned char> rb(8);
    if (!cct::web::crypto::random_bytes(rb, 8)) {
      http::respond_json(s, 500, json_err("随机数失败"));
      return;
    }
    std::string nid = cct::web::crypto::bytes_to_hex(rb.data(), rb.size());
    const std::string uk = cct::storage::user_chats_key(se->user_id);
    std::lock_guard<std::mutex> cl(chats.mutex());
    chats.ensure_loaded(se->user_id);
    cct::storage::ensure_default_thread_list_vec(chats.threads_map()[uk]);
    auto& trows = chats.threads_map()[uk];
    std::uint64_t nord = next_thread_ordinal(trows);
    trows.insert(trows.begin(), cct::storage::ChatThreadRow{nid, "新对话", 0, nord});
    chats.history_map()[cct::storage::chat_hist_key(uk, nid)] = {};
    chats.persist(se->user_id);
    http::respond_json(s, 200,
                       json_ok(",\"id\":\"" + cct::util::json_escape_string(nid) + "\",\"title\":\"新对话\""));
    return;
  }

  if (req.method == "POST" && path == "/api/chat/thread-anchor") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string tid;
    if (!cct::util::json_extract_string_after_key(req.body, "thread_id", tid) || tid.empty()) tid = "main";
    if (!valid_thread_id_str(tid)) {
      http::respond_json(s, 400, json_err("非法对话 id"));
      return;
    }
    std::string raw_anchor;
    (void)cct::util::json_extract_string_after_key(req.body, "workspace_anchor", raw_anchor);
    const std::string anchor = norm_workspace_rel_path(raw_anchor);
    if (anchor.empty()) {
      http::respond_json(s, 400, json_err("需要有效 workspace_anchor"));
      return;
    }
    const std::string uk = cct::storage::user_chats_key(se->user_id);
    {
      std::lock_guard<std::mutex> cl(chats.mutex());
      chats.ensure_loaded(se->user_id);
      cct::storage::ensure_default_thread_list_vec(chats.threads_map()[uk]);
      apply_thread_workspace_anchor(chats.threads_map()[uk], tid, anchor);
      chats.persist(se->user_id);
    }
    http::respond_json(s, 200,
                       json_ok(",\"threadId\":\"" + cct::util::json_escape_string(tid) +
                               "\",\"workspaceAnchor\":\"" + cct::util::json_escape_string(anchor) + "\""));
    return;
  }

  if (req.method == "POST" && path == "/api/chat/materialize-ide") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string label, rel_file, content;
    (void)cct::util::json_extract_string_after_key(req.body, "label", label);
    if (!cct::util::json_extract_string_after_key(req.body, "path", rel_file) || rel_file.empty()) {
      http::respond_json(s, 400, json_err("需要 path"));
      return;
    }
    if (!cct::util::json_extract_string_after_key(req.body, "content", content)) {
      http::respond_json(s, 400, json_err("需要 content"));
      return;
    }
    std::filesystem::path rel_p = std::filesystem::u8path(rel_file);
    if (!safe_rel_path(rel_p)) {
      http::respond_json(s, 400, json_err("path 含非法分量"));
      return;
    }
    std::filesystem::path parent_can;
    std::string werr;
    if (!resolve_workspace_root_canonical(llm_cfg, parent_can, werr)) {
      http::respond_json(s, 503, json_err(werr));
      return;
    }
    const std::string base = sanitize_export_folder_label(label.empty() ? std::string("CCT_Export") : label);
    std::filesystem::path folder_can;
    std::error_code ec2;
    folder_can.clear();
    for (int n = 0; n < 200; ++n) {
      const std::string seg = (n == 0 ? base : base + "_" + std::to_string(n));
      /** label/base 为 UTF-8；禁止 path(seg)，必须用 u8path 否则会落成 ANSI 乱码目录名 */
      std::filesystem::path cand = parent_can / std::filesystem::u8path(seg);
      std::error_code ec_st;
      const auto st = std::filesystem::status(cand, ec_st);
      if (!std::filesystem::exists(st)) {
        std::error_code ec_mk;
        std::filesystem::create_directories(cand, ec_mk);
        if (!ec_mk) {
          folder_can = std::move(cand);
          break;
        }
        continue;
      }
      if (std::filesystem::is_directory(st)) {
        /** 同名目录已存在：复用（同一标签重复点击「导出到 IDE」时覆盖写入，避免 _1/_2 堆砌） */
        folder_can = std::move(cand);
        break;
      }
    }
    if (folder_can.empty()) {
      http::respond_json(s, 500, json_err("无法创建导出目录"));
      return;
    }
    std::filesystem::path tgt = (folder_can / rel_p).lexically_normal();
    if (!path_is_under_root(folder_can.lexically_normal(), tgt)) {
      http::respond_json(s, 403, json_err("path 越界"));
      return;
    }
    std::filesystem::path par = tgt.parent_path();
    std::filesystem::create_directories(par, ec2);
    if (ec2) {
      http::respond_json(s, 500, json_err("创建父目录失败"));
      return;
    }
    if (content.size() > kWorkspaceMaxFile) {
      http::respond_json(s, 413, json_err("内容过大（上限 2MB）"));
      return;
    }
    std::ofstream ofs(tgt, std::ios::binary | std::ios::trunc);
    if (!ofs) {
      http::respond_json(s, 500, json_err("写入文件失败"));
      return;
    }
    ofs << content;
    std::string path2, content2;
    std::filesystem::path tgt_open = tgt;
    if (cct::util::json_extract_string_after_key(req.body, "path2", path2) && !path2.empty() &&
        cct::util::json_extract_string_after_key(req.body, "content2", content2)) {
      std::filesystem::path rel_p2 = std::filesystem::u8path(path2);
      if (!safe_rel_path(rel_p2)) {
        http::respond_json(s, 400, json_err("path2 含非法分量"));
        return;
      }
      std::filesystem::path tgt2 = (folder_can / rel_p2).lexically_normal();
      if (!path_is_under_root(folder_can.lexically_normal(), tgt2)) {
        http::respond_json(s, 403, json_err("path2 越界"));
        return;
      }
      std::filesystem::path par2 = tgt2.parent_path();
      std::filesystem::create_directories(par2, ec2);
      if (ec2) {
        http::respond_json(s, 500, json_err("创建 path2 父目录失败"));
        return;
      }
      if (content2.size() > kWorkspaceMaxFile) {
        http::respond_json(s, 413, json_err("path2 内容过大（上限 2MB）"));
        return;
      }
      std::ofstream ofs2(tgt2, std::ios::binary | std::ios::trunc);
      if (!ofs2) {
        http::respond_json(s, 500, json_err("写入 path2 失败"));
        return;
      }
      ofs2 << content2;
      tgt_open = std::move(tgt2);
    }
    std::error_code ec3, ec4;
    std::filesystem::path rel_to_parent = std::filesystem::relative(tgt_open, parent_can, ec3);
    std::string rel_s;
    if (!ec3) {
      rel_s = rel_to_parent.u8string();
    } else {
      std::filesystem::path rel_in_folder = std::filesystem::relative(tgt_open, folder_can, ec4);
      if (!ec4)
        rel_s = (folder_can.filename() / rel_in_folder).u8string();
      else
        rel_s = (folder_can.filename() / tgt_open.filename()).u8string();
    }
    for (char& c : rel_s)
      if (c == '\\') c = '/';
    http::respond_json(s, 200,
                       json_ok(std::string(",\"relPath\":\"") + cct::util::json_escape_string(rel_s) +
                               "\",\"folder\":\"" + cct::util::json_escape_string(folder_can.filename().u8string()) +
                               "\",\"root\":\"" + cct::util::json_escape_string(parent_can.u8string()) + "\""));
    return;
  }

  if (req.method == "POST" && path == "/api/chat/stream") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string msg;
    if (!cct::util::json_extract_string_after_key(req.body, "message", msg)) {
      http::respond_json(s, 400, json_err("需要 message"));
      return;
    }
    std::string editor_path, editor_content;
    const bool has_editor_ctx = cct::util::json_extract_string_after_key(req.body, "editor_content", editor_content) &&
                                !editor_content.empty();
    if (has_editor_ctx) {
      if (!cct::util::json_extract_string_after_key(req.body, "editor_path", editor_path)) editor_path.clear();
    }
    std::string workspace_bundle;
    const bool has_ws = cct::util::json_extract_string_after_key(req.body, "workspace_bundle", workspace_bundle) &&
                        workspace_bundle.size() > 8;
    std::string agent_pick, skill_pick, command_pick;
    (void)cct::util::json_extract_string_after_key(req.body, "agent", agent_pick);
    (void)cct::util::json_extract_string_after_key(req.body, "skill", skill_pick);
    (void)cct::util::json_extract_string_after_key(req.body, "command", command_pick);
    std::string client_workspace_anchor;
    (void)cct::util::json_extract_string_after_key(req.body, "workspace_anchor", client_workspace_anchor);
    const std::string uk = cct::storage::user_chats_key(se->user_id);
    std::string thread_id = parse_thread_id_json(req.body);
    std::string workspace_anchor = norm_workspace_rel_path(client_workspace_anchor);
    if (workspace_anchor.empty()) workspace_anchor = derive_workspace_anchor_from_request(editor_path, workspace_bundle);
    std::string model_pick;
    if (cct::util::json_extract_string_after_key(req.body, "model", model_pick) && !model_pick.empty()) {
      std::string prov_lc;
      for (char c : llm_cfg.llm_provider) prov_lc += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (!allowed_chat_model_for_provider(prov_lc, model_pick)) model_pick.clear();
    }
    std::vector<cct::llm::ChatMessage> hist;
    {
      std::lock_guard<std::mutex> cl(chats.mutex());
      chats.ensure_loaded(se->user_id);
      cct::storage::ensure_default_thread_list_vec(chats.threads_map()[uk]);
      auto& h = chats.history_map()[cct::storage::chat_hist_key(uk, thread_id)];
      cct::storage::apply_thread_title_from_first_user(chats.threads_map()[uk], thread_id, msg, h);
      apply_thread_workspace_anchor(chats.threads_map()[uk], thread_id, workspace_anchor);
      h.push_back(cct::llm::ChatMessage{"user", msg});
      if (h.size() > 40) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() - 40));
      hist = h;
      chats.persist(se->user_id);
    }
    std::vector<cct::llm::ChatMessage> hist_send = hist;
    if ((has_editor_ctx || has_ws) && !hist_send.empty() && hist_send.back().role == "user") {
      augment_hist_send_user_tail(hist_send, llm_cfg, editor_path, editor_content, has_editor_ctx, workspace_bundle,
                                  has_ws);
    }
    inject_user_agent_skill_prefix(hist_send, comps, se->user_id, agent_pick, skill_pick, command_pick);
    cct::util::AppConfig cfg_call = llm_cfg;
    if (!model_pick.empty()) cfg_call.model = model_pick;
    augment_chat_max_tokens_for_request(cfg_call, has_ws);

    auto rollback_user = [&]() {
      std::lock_guard<std::mutex> cl(chats.mutex());
      auto& h = chats.history_map()[cct::storage::chat_hist_key(uk, thread_id)];
      if (!h.empty() && h.back().role == "user" && h.back().content == msg) h.pop_back();
      chats.persist(se->user_id);
    };

    constexpr std::int64_t k_billing_min_chat = 512;
    std::size_t chat_apx = msg.size() + 2048;
    for (const auto& m : hist_send) chat_apx += m.content.size();
    const std::int64_t chat_pre = (std::max)(k_billing_min_chat, static_cast<std::int64_t>(chat_apx / 16));
    cct::storage::TokenBillingState bill_pre{};
    std::string bill_err;
    if (!billing.check_can_use(se->user_id, chat_pre, bill_pre, bill_err)) {
      rollback_user();
      http::respond_json(s, 429, json_err_msg_code(bill_err, "token_exhausted"));
      return;
    }
    const std::string chat_tier_n = cct::storage::billing_normalize_tier(bill_pre.tier);
    std::string chat_daily_den;
    if (tier_daily_call_blocked(data_dir, chat_tier_n, se->user_id, llm_cfg.llm_daily_call_limit, chat_daily_den)) {
      rollback_user();
      http::respond_json(s, 429, json_err_msg_code(chat_daily_den, "daily_limit_exceeded"));
      return;
    }

    auto settle_chat_tokens = [&](int pt, int ct) {
      std::size_t apx = msg.size() + 2048;
      for (const auto& m : hist_send) apx += m.content.size();
      std::int64_t d = billing_delta_from_llm_usage(pt, ct, k_billing_min_chat, apx);
      cct::storage::TokenBillingState st{};
      std::string be;
      (void)billing.add_consumed(se->user_id, d, st, be);
      record_llm_call_success_for_analytics(data_dir, se->user_id);
    };

    auto emit_sse = [&](const std::string& json_obj) {
      http::send_http_chunk(s, std::string("data: ") + json_obj + "\n\n");
    };

    http::begin_chunked_response(s, "text/event-stream; charset=utf-8");

    auto log_stream_chat = [&](bool ok_llm, std::uint64_t ms_llm, int pt, int ct, int tt) {
      analytics_emit_chat_completion(data_dir, se->user_id, thread_id, agent_pick, skill_pick, command_pick, has_ws,
                                     has_editor_ctx, model_pick, ok_llm, ms_llm, pt, ct, tt);
    };

    if (cfg_call.use_mock) {
      const std::uint64_t t0 = steady_now_ms();
      emit_sse("{\"e\":\"h\",\"d\":\"（Mock）简要推理…\"}");
      emit_sse("{\"e\":\"c\",\"d\":\"这是 Mock 流式演示回复。\"}");
      {
        std::lock_guard<std::mutex> cl(chats.mutex());
        auto& h = chats.history_map()[cct::storage::chat_hist_key(uk, thread_id)];
        h.push_back(cct::llm::ChatMessage{"assistant", "这是 Mock 流式演示回复。"});
        if (h.size() > 40) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() - 40));
        chats.persist(se->user_id);
      }
      emit_sse("{\"e\":\"done\"}");
      http::end_chunked_response(s);
      log_stream_chat(true, steady_now_ms() - t0, 0, 0, 0);
      settle_chat_tokens(0, 0);
      return;
    }

    std::string prov_lc;
    for (char c : cfg_call.llm_provider) prov_lc += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (prov_lc != "zhipu") {
      const std::uint64_t t0 = steady_now_ms();
      cct::llm::LlmResult out = cct::llm::call_anthropic_chat(cfg_call, hist_send);
      const std::uint64_t ms = steady_now_ms() - t0;
      if (!out.ok) {
        rollback_user();
        emit_sse("{\"e\":\"err\",\"m\":\"" + cct::util::json_escape_string(out.error) + "\"}");
        http::end_chunked_response(s);
        log_stream_chat(false, ms, out.usage_prompt_tokens, out.usage_completion_tokens, out.usage_total_tokens);
        return;
      }
      emit_sse("{\"e\":\"c\",\"d\":\"" + cct::util::json_escape_string(out.text) + "\"}");
      {
        std::lock_guard<std::mutex> cl(chats.mutex());
        auto& h = chats.history_map()[cct::storage::chat_hist_key(uk, thread_id)];
        h.push_back(cct::llm::ChatMessage{"assistant", out.text});
        if (h.size() > 40) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() - 40));
        chats.persist(se->user_id);
      }
      emit_sse("{\"e\":\"done\"}");
      http::end_chunked_response(s);
      log_stream_chat(true, ms, out.usage_prompt_tokens, out.usage_completion_tokens, out.usage_total_tokens);
      settle_chat_tokens(out.usage_prompt_tokens, out.usage_completion_tokens);
      return;
    }

    std::string full_c, full_h, serr;
    int zu_pt = -1, zu_ct = -1, zu_tt = -1;
    const std::uint64_t tz0 = steady_now_ms();
    bool ok = cct::llm::call_zhipu_chat_stream(
        cfg_call, hist_send,
        [&](const std::string& t, const std::string& f) {
          std::string line =
              std::string("{\"e\":\"") + (t == "h" ? "h" : "c") + "\",\"d\":\"" + cct::util::json_escape_string(f) + "\"}";
          http::send_http_chunk(s, std::string("data: ") + line + "\n\n");
          return true;
        },
        full_c, full_h, serr, &zu_pt, &zu_ct, &zu_tt);
    const std::uint64_t ms_z = steady_now_ms() - tz0;
    if (!ok) {
      rollback_user();
      emit_sse("{\"e\":\"err\",\"m\":\"" + cct::util::json_escape_string(serr) + "\"}");
      http::end_chunked_response(s);
      log_stream_chat(false, ms_z, zu_pt, zu_ct, zu_tt);
      return;
    }
    {
      std::lock_guard<std::mutex> cl(chats.mutex());
      auto& h = chats.history_map()[cct::storage::chat_hist_key(uk, thread_id)];
      h.push_back(cct::llm::ChatMessage{"assistant", full_c});
      if (h.size() > 40) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() - 40));
      chats.persist(se->user_id);
    }
    emit_sse("{\"e\":\"done\"}");
    http::end_chunked_response(s);
    log_stream_chat(true, ms_z, zu_pt, zu_ct, zu_tt);
    settle_chat_tokens(zu_pt, zu_ct);
    return;
  }

  if (req.method == "POST" && path == "/api/chat") {
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    std::string msg;
    if (!cct::util::json_extract_string_after_key(req.body, "message", msg)) {
      http::respond_json(s, 400, json_err("需要 message"));
      return;
    }
    std::string editor_path, editor_content;
    const bool has_editor_ctx = cct::util::json_extract_string_after_key(req.body, "editor_content", editor_content) &&
                                !editor_content.empty();
    if (has_editor_ctx) {
      if (!cct::util::json_extract_string_after_key(req.body, "editor_path", editor_path)) editor_path.clear();
    }
    std::string workspace_bundle;
    const bool has_ws = cct::util::json_extract_string_after_key(req.body, "workspace_bundle", workspace_bundle) &&
                        workspace_bundle.size() > 8;
    std::string agent_pick, skill_pick, command_pick;
    (void)cct::util::json_extract_string_after_key(req.body, "agent", agent_pick);
    (void)cct::util::json_extract_string_after_key(req.body, "skill", skill_pick);
    (void)cct::util::json_extract_string_after_key(req.body, "command", command_pick);
    std::string client_workspace_anchor;
    (void)cct::util::json_extract_string_after_key(req.body, "workspace_anchor", client_workspace_anchor);
    const std::string uk2 = cct::storage::user_chats_key(se->user_id);
    std::string thread_id = parse_thread_id_json(req.body);
    std::string workspace_anchor = norm_workspace_rel_path(client_workspace_anchor);
    if (workspace_anchor.empty()) workspace_anchor = derive_workspace_anchor_from_request(editor_path, workspace_bundle);
    std::string model_pick;
    if (cct::util::json_extract_string_after_key(req.body, "model", model_pick) && !model_pick.empty()) {
      std::string prov_lc;
      for (char c : llm_cfg.llm_provider) prov_lc += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (!allowed_chat_model_for_provider(prov_lc, model_pick)) model_pick.clear();
    }
    std::vector<cct::llm::ChatMessage> hist;
    {
      std::lock_guard<std::mutex> cl(chats.mutex());
      chats.ensure_loaded(se->user_id);
      cct::storage::ensure_default_thread_list_vec(chats.threads_map()[uk2]);
      auto& h = chats.history_map()[cct::storage::chat_hist_key(uk2, thread_id)];
      cct::storage::apply_thread_title_from_first_user(chats.threads_map()[uk2], thread_id, msg, h);
      apply_thread_workspace_anchor(chats.threads_map()[uk2], thread_id, workspace_anchor);
      h.push_back(cct::llm::ChatMessage{"user", msg});
      if (h.size() > 40) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() - 40));
      hist = h;
      chats.persist(se->user_id);
    }
    std::vector<cct::llm::ChatMessage> hist_send = hist;
    if ((has_editor_ctx || has_ws) && !hist_send.empty() && hist_send.back().role == "user") {
      augment_hist_send_user_tail(hist_send, llm_cfg, editor_path, editor_content, has_editor_ctx, workspace_bundle,
                                  has_ws);
    }
    inject_user_agent_skill_prefix(hist_send, comps, se->user_id, agent_pick, skill_pick, command_pick);
    cct::util::AppConfig cfg_call = llm_cfg;
    if (!model_pick.empty()) cfg_call.model = model_pick;
    augment_chat_max_tokens_for_request(cfg_call, has_ws);

    auto rollback_user_nc = [&]() {
      std::lock_guard<std::mutex> cl(chats.mutex());
      auto& h = chats.history_map()[cct::storage::chat_hist_key(uk2, thread_id)];
      if (!h.empty() && h.back().role == "user" && h.back().content == msg) h.pop_back();
      chats.persist(se->user_id);
    };

    constexpr std::int64_t k_billing_min_chat_nc = 512;
    std::size_t chat_apx_nc = msg.size() + 2048;
    for (const auto& m : hist_send) chat_apx_nc += m.content.size();
    const std::int64_t chat_pre_nc =
        (std::max)(k_billing_min_chat_nc, static_cast<std::int64_t>(chat_apx_nc / 16));
    cct::storage::TokenBillingState bill_pre_nc{};
    std::string bill_err_nc;
    if (!billing.check_can_use(se->user_id, chat_pre_nc, bill_pre_nc, bill_err_nc)) {
      rollback_user_nc();
      http::respond_json(s, 429, json_err_msg_code(bill_err_nc, "token_exhausted"));
      return;
    }
    const std::string chat_tier_nc = cct::storage::billing_normalize_tier(bill_pre_nc.tier);
    std::string daily_nc;
    if (tier_daily_call_blocked(data_dir, chat_tier_nc, se->user_id, llm_cfg.llm_daily_call_limit, daily_nc)) {
      rollback_user_nc();
      http::respond_json(s, 429, json_err_msg_code(daily_nc, "daily_limit_exceeded"));
      return;
    }

    const std::uint64_t t_chat_llm0 = steady_now_ms();
    cct::llm::LlmResult out;
    if (cfg_call.use_mock) {
      out = cct::llm::call_mock_chat(hist_send);
    } else {
      std::string prov;
      for (char c : cfg_call.llm_provider) prov += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      if (prov == "zhipu")
        out = cct::llm::call_zhipu_chat(cfg_call, hist_send);
      else
        out = cct::llm::call_anthropic_chat(cfg_call, hist_send);
    }
    const std::uint64_t chat_llm_ms = steady_now_ms() - t_chat_llm0;
    analytics_emit_chat_completion(data_dir, se->user_id, thread_id, agent_pick, skill_pick, command_pick, has_ws,
                                   has_editor_ctx, model_pick, out.ok, chat_llm_ms, out.usage_prompt_tokens,
                                   out.usage_completion_tokens, out.usage_total_tokens);
    if (!out.ok) {
      {
        std::lock_guard<std::mutex> cl(chats.mutex());
        auto& h = chats.history_map()[cct::storage::chat_hist_key(uk2, thread_id)];
        if (!h.empty() && h.back().role == "user" && h.back().content == msg) h.pop_back();
        chats.persist(se->user_id);
      }
      http::respond_json(s, 502, json_err(out.error));
      return;
    }
    {
      std::lock_guard<std::mutex> cl(chats.mutex());
      auto& h = chats.history_map()[cct::storage::chat_hist_key(uk2, thread_id)];
      h.push_back(cct::llm::ChatMessage{"assistant", out.text});
      if (h.size() > 40) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() - 40));
      chats.persist(se->user_id);
    }
    {
      std::size_t apx = msg.size() + 2048;
      for (const auto& m : hist_send) apx += m.content.size();
      std::int64_t d = billing_delta_from_llm_usage(out.usage_prompt_tokens, out.usage_completion_tokens,
                                                    k_billing_min_chat_nc, apx);
      cct::storage::TokenBillingState st{};
      std::string be;
      (void)billing.add_consumed(se->user_id, d, st, be);
    }
    record_llm_call_success_for_analytics(data_dir, se->user_id);
    http::respond_json(s, 200,
                       json_ok(",\"reply\":\"" + cct::util::json_escape_string(out.text) + "\""));
    return;
  }

  if (req.method == "POST" && path == "/api/chat/clear") {
    Session* se2 = session_from_req(req);
    if (!se2) {
      http::respond_json(s, 401, json_err("未登录"));
      return;
    }
    const std::string uk3 = cct::storage::user_chats_key(se2->user_id);
    std::string thread_id = parse_thread_id_json(req.body);
    std::lock_guard<std::mutex> cl(chats.mutex());
    chats.ensure_loaded(se2->user_id);
    chats.history_map().erase(cct::storage::chat_hist_key(uk3, thread_id));
    chats.persist(se2->user_id);
    http::respond_json(s, 200, json_ok(""));
    return;
  }

  auto component_dispatch = [&](const char* cat) -> bool {
    std::string prefix = std::string("/api/") + cat + "/";
    std::string listp = std::string("/api/") + cat;
    Session* se = session_from_req(req);
    if (!se) {
      http::respond_json(s, 401, json_err("未登录"));
      return true;
    }

    if (req.method == "POST" && path == listp) {
      std::string name, content;
      if (!cct::util::json_extract_string_after_key(req.body, "name", name) ||
          !cct::util::json_extract_string_after_key(req.body, "content", content)) {
        http::respond_json(s, 400, json_err("需要 name 与 content"));
        return true;
      }
      if (!valid_component_name(name)) {
        http::respond_json(s, 400, json_err("非法名称"));
        return true;
      }
      std::string cerr;
      if (!comps.create_new(se->user_id, cat, name, content, cerr)) {
        http::respond_json(s, 400, json_err(cerr.empty() ? "创建失败" : cerr));
        return true;
      }
      http::respond_json(s, 200, json_ok(""));
      return true;
    }

    if (req.method == "GET" && path == listp) {
      comps.ensure_default_samples(se->user_id);
      std::vector<std::string> stems;
      comps.list_stems(se->user_id, cat, stems);
      std::ostringstream arr;
      arr << "[";
      for (size_t i = 0; i < stems.size(); ++i) {
        if (i) arr << ',';
        arr << "\"" << cct::util::json_escape_string(stems[i]) << "\"";
      }
      arr << "]";
      http::respond_json(s, 200, json_ok(",\"items\":" + arr.str()));
      return true;
    }

    if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0) {
      std::string name = uri_percent_decode_utf8(path.substr(prefix.size()));
      if (!valid_component_name(name)) {
        http::respond_json(s, 400, json_err("非法名称"));
        return true;
      }
      if (req.method == "GET") {
        std::string body;
        std::string gerr;
        if (!comps.get_content(se->user_id, cat, name, body, gerr)) {
          http::respond_json(s, 404, json_err(gerr.empty() ? "不存在" : gerr));
          return true;
        }
        http::respond_json(s, 200, json_ok(",\"name\":\"" + cct::util::json_escape_string(name) +
                                                "\",\"content\":\"" + cct::util::json_escape_string(body) + "\""));
        return true;
      }
      if (req.method == "PUT" || req.method == "POST") {
        std::string content;
        if (!cct::util::json_extract_string_after_key(req.body, "content", content)) {
          http::respond_json(s, 400, json_err("需要 content"));
          return true;
        }
        std::string uerr;
        if (!comps.update_content(se->user_id, cat, name, content, uerr)) {
          http::respond_json(s, 400, json_err(uerr.empty() ? "保存失败" : uerr));
          return true;
        }
        http::respond_json(s, 200, json_ok(""));
        return true;
      }
      if (req.method == "DELETE") {
        std::string derr;
        comps.remove(se->user_id, cat, name, derr);
        (void)derr;
        http::respond_json(s, 200, json_ok(""));
        return true;
      }
    }
    return false;
  };

  if (component_dispatch("agents")) return;
  if (component_dispatch("skills")) return;
  if (component_dispatch("commands")) return;

  http::respond_json(s, 404, json_err("未知接口"));
}

void client_worker(SOCKET client, std::filesystem::path ui_root, std::filesystem::path data_dir,
                   cct::storage::IUserPersistence* users, cct::storage::IChatPersistence* chats,
                   cct::storage::IComponentPersistence* comps, cct::storage::ITokenBillingPersistence* billing,
                   cct::util::AppConfig llm_cfg) {
  http::HttpReq req;
  if (!http::read_http_request(client, req)) {
    closesocket(client);
    return;
  }
  if (req.path.rfind("/api/", 0) == 0) {
    if (!billing) {
      http::respond_json(client, 503, json_err("计费模块未初始化"));
      closesocket(client);
      return;
    }
    handle_api(client, req, *users, *chats, *comps, *billing, data_dir, llm_cfg);
  } else {
    handle_static(client, ui_root, req.path);
  }
  closesocket(client);
}

}  // namespace

void run_http_server(std::uint16_t port, const std::filesystem::path& ui_root,
                     const std::filesystem::path& data_dir, const cct::util::AppConfig& llm_cfg) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::cerr << "WSAStartup 失败\n";
    return;
  }
  SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock == INVALID_SOCKET) {
    std::cerr << "socket 失败\n";
    WSACleanup();
    return;
  }
  BOOL opt = TRUE;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  /** 监听 0.0.0.0：避免部分环境下 localhost 解析到 ::1 而仅绑定 127.0.0.1 导致浏览器 fetch 失败（历史对话等接口报「网络错误」） */
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "bind 0.0.0.0:" << port << " 失败\n";
    closesocket(listen_sock);
    WSACleanup();
    return;
  }
  if (listen(listen_sock, SOMAXCONN) != 0) {
    std::cerr << "listen 失败\n";
    closesocket(listen_sock);
    WSACleanup();
    return;
  }

  void* sql_bundle = nullptr;
  cct::storage::IUserPersistence* user_api = nullptr;
  cct::storage::IChatPersistence* chat_api = nullptr;
  cct::storage::IComponentPersistence* comp_api = nullptr;
  cct::storage::ITokenBillingPersistence* billing_api = nullptr;

  if (llm_cfg.sql_odbc_connection_string.empty()) {
    std::cerr << "未配置 SQL Server：请在 config.json 中填写 sql_odbc_connection_string，并先执行 scripts/sql/schema.sql。\n";
    closesocket(listen_sock);
    WSACleanup();
    return;
  }
  char errbuf[512]{};
  sql_bundle = cct_sql_bundle_open(llm_cfg.sql_odbc_connection_string.c_str(), errbuf, sizeof errbuf);
  if (!sql_bundle) {
    std::cerr << "SQL Server 打开失败: " << errbuf << "\n";
    closesocket(listen_sock);
    WSACleanup();
    return;
  }
  user_api = cct_sql_bundle_users(sql_bundle);
  chat_api = cct_sql_bundle_chats(sql_bundle);
  comp_api = cct_sql_bundle_components(sql_bundle);
  billing_api = cct_sql_bundle_token_billing(sql_bundle);

  std::cout << "cct-cn Web: http://127.0.0.1:" << port << "/ （亦可用 http://localhost:" << port << "/）\n";
  std::cout << "静态目录: " << ui_root.string() << "\n";
  std::cout << "持久化: SQL Server (ODBC)\n";
  {
    std::string prov;
    for (char c : llm_cfg.llm_provider) prov += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    std::cout << "LLM: ";
    if (llm_cfg.use_mock)
      std::cout << "Mock（未请求外网）";
    else if (prov == "zhipu")
      std::cout << "智谱，模型 " << llm_cfg.model << "，主机 " << llm_cfg.api_host;
    else
      std::cout << "Anthropic，模型 " << llm_cfg.model << "，主机 " << llm_cfg.api_host;
    std::cout << "\n";
  }
  if (!llm_cfg.workspace_root.empty()) {
    std::cout << "IDE 工作区(workspace_root): " << llm_cfg.workspace_root << "\n";
  }
  {
    std::filesystem::path exe_dir = ui_root.parent_path();
    if (llm_cfg.git_worker_autostart && llm_cfg.git_worker_port > 0 && llm_cfg.git_worker_port <= 65535) {
      std::string sp_err;
      if (!cct::web::spawn_git_worker(exe_dir, llm_cfg.git_worker_port, sp_err)) {
        std::cerr << "[git-worker] " << sp_err << "\n";
      } else {
        std::cout << "Git 附属服务 git-worker: http://127.0.0.1:" << llm_cfg.git_worker_port << "/\n";
      }
    }
  }
  for (;;) {
    SOCKET c = accept(listen_sock, nullptr, nullptr);
    if (c == INVALID_SOCKET) continue;
    std::thread(client_worker, c, ui_root, data_dir, user_api, chat_api, comp_api, billing_api, llm_cfg).detach();
  }
}

}  // namespace cct::web

#endif
