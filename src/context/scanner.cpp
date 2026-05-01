#include "scanner.hpp"

#include <fstream>
#include <set>
#include <sstream>

namespace cct::context {

static bool is_ignored_dir(const std::string& name, const std::set<std::string>& base) {
  return base.count(name) > 0;
}

static const std::set<std::string>& default_ignores() {
  static const std::set<std::string> s = {
    ".git", ".svn", ".hg", "node_modules", "build", "out", "dist", ".vs", ".idea",
    "target", "cmake-build-debug", "cmake-build-release", "__pycache__", ".cct-cn"};
  return s;
}

static bool is_probably_text(const std::filesystem::path& p) {
  static const std::set<std::string> ext = {
    ".md", ".txt", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".c", ".js", ".ts", ".tsx", ".jsx",
    ".json", ".yaml", ".yml", ".toml", ".cmake", ".py", ".java", ".go", ".rs", ".cs", ".vue", ".html", ".css"};
  std::string e = p.extension().string();
  for (auto& ch : e) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
  return ext.count(e) > 0;
}

std::string scan_project_context(const ScanOptions& opt, std::string& error) {
  std::error_code ec;
  if (!std::filesystem::exists(opt.root, ec) || !std::filesystem::is_directory(opt.root, ec)) {
    error = "项目路径不是有效目录: " + opt.root.string();
    return {};
  }
  std::set<std::string> ignore = default_ignores();
  for (const auto& x : opt.extra_ignore_dir_names) ignore.insert(x);

  std::ostringstream out;
  int used = 0;
  const int budget = opt.max_total_chars > 0 ? opt.max_total_chars : 12000;

  try {
    for (auto it = std::filesystem::recursive_directory_iterator(
             opt.root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (ec) break;
      const auto& p = it->path();
      if (it->is_directory()) {
        std::string dirname = p.filename().string();
        if (is_ignored_dir(dirname, ignore)) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (!it->is_regular_file()) continue;
      if (!is_probably_text(p)) continue;
      auto rel = std::filesystem::relative(p, opt.root, ec);
      if (ec) continue;
      std::ifstream f(p, std::ios::binary);
      if (!f) continue;
      std::stringstream sb;
      sb << f.rdbuf();
      std::string content = sb.str();
      if (content.size() > 8000) content = content.substr(0, 8000) + "\n... [已截断] ...\n";
      std::string header = "\n\n===== 文件: " + rel.generic_string() + " =====\n";
      int chunk = static_cast<int>(header.size() + content.size());
      if (used + chunk > budget) {
        out << "\n[上下文已达上限，后续文件已省略]\n";
        break;
      }
      used += chunk;
      out << header << content;
    }
  } catch (const std::exception& e) {
    error = std::string("扫描目录异常: ") + e.what();
    return {};
  }
  return out.str();
}

}
