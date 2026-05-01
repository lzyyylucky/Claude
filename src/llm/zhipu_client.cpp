#include "api.hpp"

#include "../util/config.hpp"
#include "../util/json_minimal.hpp"

#include <cctype>
#include <functional>
#include <sstream>

#ifndef _WIN32

namespace cct::llm {

bool call_zhipu_chat_stream(const cct::util::AppConfig&, const std::vector<ChatMessage>&,
                            const std::function<bool(const std::string&, const std::string&)>&,
                            std::string&, std::string&, std::string& error, int*, int*, int*) {
  error = "智谱流式仅支持 Windows（WinHTTP）。";
  return false;
}

LlmResult call_zhipu(const cct::util::AppConfig&, const std::string&) {
  LlmResult r;
  r.ok = false;
  r.error = "智谱 HTTPS 仅在 Windows（WinHTTP）下实现；请使用 Windows 构建或 use_mock=true。";
  return r;
}

LlmResult call_zhipu_chat(const cct::util::AppConfig&, const std::vector<ChatMessage>&) {
  LlmResult r;
  r.ok = false;
  r.error = "智谱 HTTPS 仅在 Windows（WinHTTP）下实现；请使用 Windows 构建或 use_mock=true。";
  return r;
}

}  // namespace cct::llm

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

namespace cct::llm {

namespace {

#ifndef WINHTTP_OPTION_EXTENDED_ERROR
#define WINHTTP_OPTION_EXTENDED_ERROR 24
#endif

static void append_win32_error(std::string& msg) {
  DWORD e = GetLastError();
  if (e == 0) {
    msg +=
        "（GetLastError 为 0：常见于 HTTPS 被解密类代理/安全软件插入证书、TLS 握手被重置；可尝试关闭系统代理、退出抓包/杀毒 "
        "HTTPS 扫描，或换手机热点网络）";
    return;
  }
  msg += "，Win32 错误码 ";
  msg += std::to_string(e);
  LPSTR buf = nullptr;
  DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buf), 0,
                             nullptr);
  if (n != 0 && buf) {
    msg += "：";
    msg.append(buf, n);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) msg.pop_back();
  }
  if (buf) LocalFree(buf);
}

static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

static std::string tolower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return s;
}

static LlmResult zhipu_post_json(const cct::util::AppConfig& cfg, std::string json_str) {
  LlmResult r;
  /* DEFAULT_PROXY 在部分环境不会走系统代理/自动检测，易导致 TLS 异常；AUTOMATIC_PROXY 更贴近浏览器行为 */
#if defined(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)
  const DWORD kAccess = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#else
  const DWORD kAccess = 4; /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */
#endif
  HINTERNET hSession = WinHttpOpen(L"cct-cn/0.1", kAccess, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    r.error = "WinHttpOpen 失败";
    append_win32_error(r.error);
    return r;
  }

  {
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    protos |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    (void)WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
  }

  std::wstring host = utf8_to_wide(cfg.api_host);
  std::wstring path = utf8_to_wide(cfg.api_path);
  HINTERNET hConnect =
      WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    r.error = "WinHttpConnect 失败";
    append_win32_error(r.error);
    return r;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    r.error = "WinHttpOpenRequest 失败";
    append_win32_error(r.error);
    return r;
  }

#if defined(WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL) && defined(WINHTTP_PROTOCOL_FLAG_HTTP1_1)
  {
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP1_1;
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
  }
#endif
#if defined(WINHTTP_OPTION_DECOMPRESSION) && defined(WINHTTP_DECOMPRESSION_FLAG_ALL)
  {
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));
  }
#endif
  {
    DWORD ms = 120000;
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
  }

  std::string hdrs;
  hdrs += "Content-Type: application/json\r\n";
  hdrs += "Authorization: Bearer " + cfg.api_key + "\r\n";
  std::wstring wh = utf8_to_wide(hdrs);

  BOOL sent = WinHttpSendRequest(hRequest, wh.c_str(), static_cast<DWORD>(-1),
                                 json_str.empty() ? WINHTTP_NO_REQUEST_DATA
                                                  : reinterpret_cast<LPVOID>(&json_str[0]),
                                 static_cast<DWORD>(json_str.size()),
                                 static_cast<DWORD>(json_str.size()), 0);
  if (!sent) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    r.error = "WinHttpSendRequest 失败";
    append_win32_error(r.error);
    return r;
  }

  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    std::string err = "WinHttpReceiveResponse 失败（TLS/代理/防火墙或远端重置连接）";
    DWORD ext = 0;
    DWORD el = sizeof(ext);
    if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_EXTENDED_ERROR, &ext, &el) && ext != 0) {
      err += "，WinHTTP 扩展错误 ";
      err += std::to_string(ext);
    }
    append_win32_error(err);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    r.error = std::move(err);
    return r;
  }

  DWORD status = 0;
  DWORD sz = sizeof(status);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                       &status, &sz, WINHTTP_NO_HEADER_INDEX);

  std::string response;
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
    std::string chunk(static_cast<size_t>(avail), '\0');
    DWORD read = 0;
    if (!WinHttpReadData(hRequest, &chunk[0], avail, &read)) break;
    chunk.resize(read);
    response += chunk;
  }
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  if (status != 200) {
    r.ok = false;
    std::string msg;
    if (cct::util::json_extract_string_after_key(response, "message", msg)) {
      r.error = "HTTP " + std::to_string(status) + " — " + msg;
    } else {
      r.error = "HTTP " + std::to_string(status) + " 响应: " + response.substr(0, 800);
    }
    return r;
  }

  std::string text;
  std::string err;
  if (cct::util::openai_style_extract_assistant_content(response, text, err)) {
    r.ok = true;
    r.text = std::move(text);
    (void)cct::util::json_try_extract_llm_usage_tokens(response, r.usage_prompt_tokens, r.usage_completion_tokens,
                                                       r.usage_total_tokens);
    return r;
  }
  r.error = err.empty() ? "无法解析智谱模型输出" : err;
  return r;
}

static const char* kZhipuSystemChat =
    "你是毕设项目「Claudecode」中的编程助手，回答简洁专业。用户可能在 Web IDE 中打开了文件；若需把工作区里的文件替换为"
    "新全文，可在回复最后一行单独输出一行 CCT_APPLY: 紧接 JSON：{\"path\":\"相对路径\",\"content\":\"全文\"}，"
    "服务端会写入并打开该文件。"
    "若用户消息里含工作区多文件快照，你必须在**最终可见正文**末尾输出 CCT_WORKSPACE: 及 {\"writes\":[...]} 才会真正写盘；"
    "不要只在自然语言里写「已创建/已修改」却省略该 JSON。";

static void append_messages_json(std::ostringstream& j, const std::vector<ChatMessage>& messages, bool prepend_system,
                                 const char* system_text) {
  j << '[';
  bool first = true;
  if (prepend_system && system_text) {
    j << "{\"role\":\"system\",\"content\":\"" << cct::util::json_escape_string(system_text) << "\"}";
    first = false;
  }
  for (const auto& m : messages) {
    std::string rl = tolower_ascii(m.role);
    if (rl != "user" && rl != "assistant" && rl != "system") rl = "user";
    if (rl == "system" && prepend_system) continue;
    if (!first) j << ',';
    first = false;
    j << "{\"role\":\"" << rl << "\",\"content\":\"" << cct::util::json_escape_string(m.content) << "\"}";
  }
  j << ']';
}

static bool zhipu_extract_stream_payload(const std::string& payload, std::string& reasoning_piece,
                                         std::string& content_piece) {
  reasoning_piece.clear();
  content_piece.clear();
  if (payload.empty() || payload == "[DONE]") return true;
  (void)cct::util::json_extract_string_after_key(payload, "reasoning_content", reasoning_piece);
  (void)cct::util::json_extract_string_after_key(payload, "content", content_piece);
  return true;
}

/** 流式：读 SSE data 行，聚合 acc；每段调用 on_delta("h"|"c", fragment)，返回 false 中止 */
static bool zhipu_chat_stream_http(const cct::util::AppConfig& cfg, const std::string& json_body,
                                   const std::function<bool(const std::string&, const std::string&)>& on_delta,
                                   std::string& acc_c, std::string& acc_h, std::string& error,
                                   int* usage_prompt_tokens, int* usage_completion_tokens, int* usage_total_tokens) {
  acc_c.clear();
  acc_h.clear();
  if (usage_prompt_tokens) *usage_prompt_tokens = -1;
  if (usage_completion_tokens) *usage_completion_tokens = -1;
  if (usage_total_tokens) *usage_total_tokens = -1;
#if defined(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)
  const DWORD kAccess = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#else
  const DWORD kAccess = 4;
#endif
  HINTERNET hSession = WinHttpOpen(L"cct-cn/0.1", kAccess, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    error = "WinHttpOpen 失败";
    append_win32_error(error);
    return false;
  }
  {
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    protos |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    (void)WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
  }
  std::wstring host = utf8_to_wide(cfg.api_host);
  std::wstring path = utf8_to_wide(cfg.api_path);
  HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    error = "WinHttpConnect 失败";
    append_win32_error(error);
    return false;
  }
  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    error = "WinHttpOpenRequest 失败";
    append_win32_error(error);
    return false;
  }
#if defined(WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL) && defined(WINHTTP_PROTOCOL_FLAG_HTTP1_1)
  {
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP1_1;
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
  }
#endif
#if defined(WINHTTP_OPTION_DECOMPRESSION) && defined(WINHTTP_DECOMPRESSION_FLAG_ALL)
  {
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));
  }
#endif
  {
    DWORD ms = 120000;
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    (void)WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
  }
  std::string hdrs;
  hdrs += "Content-Type: application/json\r\n";
  hdrs += "Authorization: Bearer " + cfg.api_key + "\r\n";
  std::wstring wh = utf8_to_wide(hdrs);
  std::string json_str = json_body;
  if (!WinHttpSendRequest(hRequest, wh.c_str(), static_cast<DWORD>(-1),
                          json_str.empty() ? WINHTTP_NO_REQUEST_DATA : reinterpret_cast<LPVOID>(&json_str[0]),
                          static_cast<DWORD>(json_str.size()), static_cast<DWORD>(json_str.size()), 0)) {
    error = "WinHttpSendRequest 失败";
    append_win32_error(error);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }
  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    error = "WinHttpReceiveResponse 失败";
    DWORD ext = 0;
    DWORD el = sizeof(ext);
    if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_EXTENDED_ERROR, &ext, &el) && ext != 0) {
      error += "，扩展错误 ";
      error += std::to_string(ext);
    }
    append_win32_error(error);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }
  DWORD status = 0;
  DWORD sz = sizeof(status);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                       &status, &sz, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    std::string response;
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
      std::string chunk(static_cast<size_t>(avail), '\0');
      DWORD read = 0;
      if (!WinHttpReadData(hRequest, &chunk[0], avail, &read)) break;
      chunk.resize(read);
      response += chunk;
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    std::string msg;
    if (cct::util::json_extract_string_after_key(response, "message", msg))
      error = "HTTP " + std::to_string(status) + " — " + msg;
    else
      error = "HTTP " + std::to_string(status) + " 响应: " + response.substr(0, 800);
    return false;
  }

  std::string carry;
  char tmp[16384];
  for (;;) {
    DWORD nr = 0;
    if (!WinHttpReadData(hRequest, tmp, sizeof(tmp), &nr) || nr == 0) break;
    carry.append(tmp, static_cast<size_t>(nr));
    for (;;) {
      size_t nl = carry.find('\n');
      if (nl == std::string::npos) break;
      std::string line = carry.substr(0, nl);
      carry.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line.erase(line.begin());
      if (line.rfind("data:", 0) != 0) continue;
      std::string payload = line.substr(5);
      while (!payload.empty() && (payload[0] == ' ' || payload[0] == '\t')) payload.erase(payload.begin());
      std::string rp, cp;
      zhipu_extract_stream_payload(payload, rp, cp);
      {
        int upt = -1, uct = -1, utt = -1;
        if (cct::util::json_try_extract_llm_usage_tokens(payload, upt, uct, utt)) {
          if (usage_prompt_tokens && upt >= 0) *usage_prompt_tokens = upt;
          if (usage_completion_tokens && uct >= 0) *usage_completion_tokens = uct;
          if (usage_total_tokens && utt >= 0) *usage_total_tokens = utt;
        }
      }
      if (!rp.empty()) {
        acc_h += rp;
        if (!on_delta("h", rp)) {
          WinHttpCloseHandle(hRequest);
          WinHttpCloseHandle(hConnect);
          WinHttpCloseHandle(hSession);
          return true;
        }
      }
      if (!cp.empty()) {
        acc_c += cp;
        if (!on_delta("c", cp)) {
          WinHttpCloseHandle(hRequest);
          WinHttpCloseHandle(hConnect);
          WinHttpCloseHandle(hSession);
          return true;
        }
      }
    }
  }
  if (!carry.empty()) {
    std::string line = carry;
    while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line.erase(line.begin());
    if (line.rfind("data:", 0) == 0) {
      std::string payload = line.substr(5);
      while (!payload.empty() && (payload[0] == ' ' || payload[0] == '\t')) payload.erase(payload.begin());
      std::string rp, cp;
      zhipu_extract_stream_payload(payload, rp, cp);
      {
        int upt = -1, uct = -1, utt = -1;
        if (cct::util::json_try_extract_llm_usage_tokens(payload, upt, uct, utt)) {
          if (usage_prompt_tokens && upt >= 0) *usage_prompt_tokens = upt;
          if (usage_completion_tokens && uct >= 0) *usage_completion_tokens = uct;
          if (usage_total_tokens && utt >= 0) *usage_total_tokens = utt;
        }
      }
      if (!rp.empty()) {
      }
      if (!cp.empty()) {
        acc_c += cp;
        (void)on_delta("c", cp);
      }
    }
  }
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return true;
}

}  // namespace

LlmResult call_zhipu(const cct::util::AppConfig& cfg, const std::string& user_prompt) {
  LlmResult r;
  if (cfg.api_key.empty()) {
    r.error = "api_key 为空，请在 .cct-cn/config.json 中填入智谱开放平台 API Key。";
    return r;
  }
  std::ostringstream j;
  j << "{\"model\":\"" << cct::util::json_escape_string(cfg.model) << "\",\"max_tokens\":" << cfg.max_tokens
    << ",\"messages\":[{\"role\":\"user\",\"content\":\"" << cct::util::json_escape_string(user_prompt) << "\"}]}";
  return zhipu_post_json(cfg, j.str());
}

LlmResult call_zhipu_chat(const cct::util::AppConfig& cfg, const std::vector<ChatMessage>& messages) {
  LlmResult r;
  if (cfg.api_key.empty()) {
    r.error = "api_key 为空，请在 .cct-cn/config.json 中填入智谱开放平台 API Key。";
    return r;
  }
  if (messages.empty()) {
    r.error = "messages 为空";
    return r;
  }
  std::ostringstream j;
  j << "{\"model\":\"" << cct::util::json_escape_string(cfg.model) << "\",\"max_tokens\":" << cfg.max_tokens
    << ",\"messages\":";
  append_messages_json(j, messages, true, kZhipuSystemChat);
  j << "}";
  return zhipu_post_json(cfg, j.str());
}

bool call_zhipu_chat_stream(const cct::util::AppConfig& cfg, const std::vector<ChatMessage>& messages,
                            const std::function<bool(const std::string& type, const std::string& fragment)>& on_delta,
                            std::string& out_full_content, std::string& out_full_thinking, std::string& error,
                            int* usage_prompt_tokens, int* usage_completion_tokens, int* usage_total_tokens) {
  if (cfg.api_key.empty()) {
    error = "api_key 为空，请在 .cct-cn/config.json 中填入智谱开放平台 API Key。";
    return false;
  }
  if (messages.empty()) {
    error = "messages 为空";
    return false;
  }
  std::ostringstream j;
  j << "{\"model\":\"" << cct::util::json_escape_string(cfg.model) << "\",\"max_tokens\":" << cfg.max_tokens
    << ",\"messages\":";
  append_messages_json(j, messages, true, kZhipuSystemChat);
  j << ",\"stream\":true,\"thinking\":{\"type\":\"enabled\"}}";
  std::string acc_c, acc_h;
  if (!zhipu_chat_stream_http(cfg, j.str(), on_delta, acc_c, acc_h, error, usage_prompt_tokens,
                              usage_completion_tokens, usage_total_tokens))
    return false;
  out_full_content = std::move(acc_c);
  out_full_thinking = std::move(acc_h);
  return true;
}

}  // namespace cct::llm

#endif
