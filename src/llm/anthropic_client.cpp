#include "api.hpp"

#include "../util/config.hpp"
#include "../util/json_minimal.hpp"

#include <sstream>

#ifndef _WIN32

namespace cct::llm {

LlmResult call_anthropic(const cct::util::AppConfig&, const std::string&) {
  LlmResult r;
  r.ok = false;
  r.error =
      "Anthropic HTTPS 仅在 Windows（WinHTTP）下实现；请使用 Windows 构建或后续接入 libcurl；也可 use_mock=true。";
  return r;
}

LlmResult call_anthropic_chat(const cct::util::AppConfig&, const std::vector<ChatMessage>&) {
  LlmResult r;
  r.ok = false;
  r.error =
      "Anthropic HTTPS 仅在 Windows（WinHTTP）下实现；请使用 Windows 构建或后续接入 libcurl；也可 use_mock=true。";
  return r;
}

}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

namespace cct::llm {

static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

static LlmResult anthropic_post_json(const cct::util::AppConfig& cfg, std::string json_str) {
  LlmResult r;
#if defined(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)
  const DWORD kAccess = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#else
  const DWORD kAccess = 4;
#endif
  HINTERNET hSession = WinHttpOpen(L"cct-cn/0.1", kAccess, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    r.error = "WinHttpOpen 失败";
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
    return r;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    r.error = "WinHttpOpenRequest 失败";
    return r;
  }

  std::string hdrs;
  hdrs += "Content-Type: application/json\r\n";
  hdrs += "x-api-key: " + cfg.api_key + "\r\n";
  hdrs += "anthropic-version: 2023-06-01\r\n";
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
    return r;
  }

  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    r.error = "WinHttpReceiveResponse 失败";
    return r;
  }

  DWORD status = 0;
  DWORD sz = sizeof(status);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                       WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

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
  if (cct::util::anthropic_extract_first_text(response, text, err)) {
    r.ok = true;
    r.text = std::move(text);
    (void)cct::util::json_try_extract_llm_usage_tokens(response, r.usage_prompt_tokens, r.usage_completion_tokens,
                                                       r.usage_total_tokens);
    return r;
  }
  r.error = err.empty() ? "无法解析模型输出" : err;
  return r;
}

LlmResult call_anthropic(const cct::util::AppConfig& cfg, const std::string& user_prompt) {
  LlmResult r;
  if (cfg.api_key.empty()) {
    r.error = "api_key 为空，请在 .cct-cn/config.json 中配置。";
    return r;
  }
  std::string esc = cct::util::json_escape_string(user_prompt);
  std::string esc_model = cct::util::json_escape_string(cfg.model);
  std::string json_str = std::string("{\"model\":\"") + esc_model + "\",\"max_tokens\":" +
                         std::to_string(cfg.max_tokens) + ",\"messages\":[{\"role\":\"user\",\"content\":\"" +
                         esc + "\"}]}";
  return anthropic_post_json(cfg, json_str);
}

LlmResult call_anthropic_chat(const cct::util::AppConfig& cfg, const std::vector<ChatMessage>& messages) {
  LlmResult r;
  if (cfg.api_key.empty()) {
    r.error = "api_key 为空，请在 .cct-cn/config.json 中配置。";
    return r;
  }
  if (messages.empty()) {
    r.error = "messages 为空";
    return r;
  }
  std::ostringstream j;
  j << "{\"model\":\"" << cct::util::json_escape_string(cfg.model) << "\",\"max_tokens\":" << cfg.max_tokens
    << ",\"messages\":[";
  for (size_t i = 0; i < messages.size(); ++i) {
    if (i) j << ',';
    std::string role = messages[i].role;
    if (role != "user" && role != "assistant") role = "user";
    j << "{\"role\":\"" << role << "\",\"content\":\"" << cct::util::json_escape_string(messages[i].content)
      << "\"}";
  }
  j << "]}";
  return anthropic_post_json(cfg, j.str());
}

}

#endif
