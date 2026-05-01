#include "git_worker_client.hpp"

#ifndef _WIN32

namespace cct::web {

bool git_worker_http_post(int, const std::string&, const std::string&, int, int&, std::string&, std::string&) {
  return false;
}

bool spawn_git_worker(const std::filesystem::path&, int, std::string& error_out) {
  error_out = "非 Windows 未实现 git-worker";
  return false;
}

}  // namespace cct::web

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace cct::web {

namespace {

std::wstring utf8_to_wide(const std::string& u8) {
  if (u8.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), &w[0], n);
  return w;
}

/** PATH 未包含 node 时（例如从 VS 调试启动），尝试常见安装路径 */
std::wstring resolve_node_exe_path() {
  wchar_t from_search[MAX_PATH * 4]{};
  DWORD nlen = SearchPathW(nullptr, L"node.exe", nullptr, static_cast<DWORD>(std::size(from_search)),
                             from_search, nullptr);
  if (nlen > 0 && nlen < std::size(from_search)) return std::wstring(from_search);

  auto exists_file = [](const std::wstring& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(p), ec);
  };

  static constexpr const wchar_t* k_fixed[] = {
      LR"(C:\Program Files\nodejs\node.exe)",
      LR"(C:\Program Files (x86)\nodejs\node.exe)",
  };
  for (const wchar_t* c : k_fixed) {
    std::wstring w(c);
    if (exists_file(w)) return w;
  }

  wchar_t buf[MAX_PATH]{};
  DWORD n = GetEnvironmentVariableW(L"ProgramFiles", buf, static_cast<DWORD>(std::size(buf)));
  if (n > 0 && n < std::size(buf)) {
    std::wstring p = std::wstring(buf) + L"\\nodejs\\node.exe";
    if (exists_file(p)) return p;
  }
  n = GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, static_cast<DWORD>(std::size(buf)));
  if (n > 0 && n < std::size(buf)) {
    std::wstring p = std::wstring(buf) + L"\\nodejs\\node.exe";
    if (exists_file(p)) return p;
  }
  return {};
}

}  // namespace

bool git_worker_http_post(int port, const std::string& path_utf8, const std::string& json_body_utf8,
                          int timeout_ms, int& http_status_out, std::string& response_body_out,
                          std::string& error_out) {
  http_status_out = 0;
  response_body_out.clear();
  error_out.clear();
  if (port <= 0 || port > 65535) {
    error_out = "端口无效";
    return false;
  }
  if (path_utf8.empty() || path_utf8[0] != '/') {
    error_out = "path 必须以 / 开头";
    return false;
  }

  const DWORD access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
  HINTERNET hSession =
      WinHttpOpen(L"cct-cn-git-proxy/1", access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    error_out = "WinHttpOpen 失败";
    return false;
  }

  DWORD ms = static_cast<DWORD>((std::max)(timeout_ms, 1000));
  (void)WinHttpSetTimeouts(hSession, ms, ms, ms, ms);

  HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    error_out = "无法连接 git-worker（请先 npm install 于 git-worker 并启动 cct-cn，或手动 node git-worker/index.js）";
    return false;
  }

  std::wstring wpath = utf8_to_wide(path_utf8);
  HINTERNET hRequest =
      WinHttpOpenRequest(hConnect, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    error_out = "WinHttpOpenRequest 失败";
    return false;
  }

  static const wchar_t kHdr[] =
      L"Content-Type: application/json; charset=utf-8\r\n"
      L"Accept: application/json\r\n";
  BOOL sent = WinHttpSendRequest(hRequest, kHdr, static_cast<DWORD>(-1),
                                 json_body_utf8.empty() ? WINHTTP_NO_REQUEST_DATA
                                                        : const_cast<char*>(json_body_utf8.data()),
                                 json_body_utf8.empty() ? 0 : static_cast<DWORD>(json_body_utf8.size()),
                                 json_body_utf8.empty() ? 0 : static_cast<DWORD>(json_body_utf8.size()), 0);
  if (!sent) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    error_out = "WinHttpSendRequest 失败";
    return false;
  }

  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    error_out = "WinHttpReceiveResponse 失败";
    return false;
  }

  DWORD status = 0;
  DWORD sz = sizeof(status);
  if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
    status = 502;
  }
  http_status_out = static_cast<int>(status);

  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
    std::vector<char> chunk(static_cast<size_t>(avail) + 1);
    DWORD read = 0;
    if (!WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0) break;
    response_body_out.append(chunk.data(), chunk.data() + read);
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return true;
}

bool spawn_git_worker(const std::filesystem::path& exe_dir, int port, std::string& error_out) {
  error_out.clear();
  std::filesystem::path script = exe_dir / "git-worker" / "index.js";
  std::error_code ec;
  if (!std::filesystem::exists(script, ec)) {
    error_out = "未找到 " + script.string();
    return false;
  }

  std::wstring node_exe = resolve_node_exe_path();
  if (node_exe.empty()) {
    error_out =
        "未找到 node.exe。请安装 Node.js，或将「安装目录」（通常为 C:\\Program Files\\nodejs）加入系统 "
        "环境变量 PATH 后重启 Visual Studio。";
    return false;
  }

  std::wstring wscript = script.native();
  std::wstring args = L"\"" + node_exe + L"\" \"" + wscript + L"\" --port " + std::to_wstring(port);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;

  std::vector<wchar_t> cmdbuf(args.begin(), args.end());
  cmdbuf.push_back(L'\0');

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE,
                           CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
                           nullptr, exe_dir.c_str(), &si, &pi);
  if (!ok) {
    error_out = "CreateProcessW 启动 git-worker 失败";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  Sleep(400);
  return true;
}

}  // namespace cct::web

#endif
