#include "writer.hpp"

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace cct::generator {

static std::string trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.pop_back();
  return s;
}

static bool looks_like_path(const std::string& first_line) {
  if (first_line.empty()) return false;
  if (first_line.find('/') != std::string::npos) return true;
  if (first_line.find('\\') != std::string::npos) return true;
  if (first_line.find('.') != std::string::npos) {
    static const char* langs[] = {"cpp", "c", "cc", "cxx", "h", "hpp", "py", "js", "ts", "tsx",
                                  "jsx", "json", "md", "txt", "html", "css", "vue", "go", "rs", "java"};
    for (auto* l : langs) {
      if (first_line == l) return false;
    }
    return true;
  }
  return false;
}

ApplyResult apply_model_output(const std::string& model_text, const std::filesystem::path& base_out,
                               bool dry_run, bool force) {
  ApplyResult res;
  std::regex re(R"(```([^\r\n`]*)\r?\n([\s\S]*?)```)");
  auto begin = std::sregex_iterator(model_text.begin(), model_text.end(), re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    std::string meta = trim((*it)[1].str());
    std::string code = (*it)[2].str();
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
    if (meta.empty()) {
      res.messages.push_back("跳过无元信息的代码块");
      continue;
    }
    if (!looks_like_path(meta)) {
      res.messages.push_back("跳过语言类代码块: " + meta);
      continue;
    }
    auto out_path = base_out / std::filesystem::path(meta);
    std::error_code ec;
    if (dry_run) {
      res.messages.push_back("[dry-run] 将写入: " + out_path.generic_string());
      continue;
    }
    std::filesystem::create_directories(out_path.parent_path(), ec);
    if (ec) {
      res.messages.push_back("创建目录失败: " + ec.message());
      continue;
    }
    if (std::filesystem::exists(out_path) && !force) {
      res.messages.push_back("已存在，跳过（加 --force 覆盖）: " + out_path.generic_string());
      continue;
    }
    std::ofstream f(out_path, std::ios::binary);
    if (!f) {
      res.messages.push_back("无法写入: " + out_path.string());
      continue;
    }
    f << code;
    res.files_written++;
    res.messages.push_back("已写入: " + out_path.generic_string());
  }
  if (res.files_written == 0 && res.messages.empty()) {
    res.messages.push_back("未解析到带路径的 Markdown 代码块，完整输出如下:\n" + model_text);
  }
  return res;
}

}
