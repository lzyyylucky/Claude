#pragma once

#include <filesystem>
#include <string>

namespace cct::web {

/** POST JSON to http://127.0.0.1:{port}{path}（path 如 "/git/status"），返回 HTTP 状态码与响应体（UTF-8）。 */
bool git_worker_http_post(int port, const std::string& path_utf8, const std::string& json_body_utf8,
                          int timeout_ms, int& http_status_out, std::string& response_body_out,
                          std::string& error_out);

/** 尝试启动 git-worker（node git-worker/index.js --port N）。exe_dir 为 cct-cn.exe 所在目录。 */
bool spawn_git_worker(const std::filesystem::path& exe_dir, int port, std::string& error_out);

}  // namespace cct::web
