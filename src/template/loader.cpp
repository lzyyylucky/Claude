#include "loader.hpp"

#include <fstream>
#include <sstream>

namespace cct::template_ns {

static std::string trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.pop_back();
  return s;
}

static void strip_utf8_bom(std::string& s) {
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xef && static_cast<unsigned char>(s[1]) == 0xbb &&
      static_cast<unsigned char>(s[2]) == 0xbf) {
    s.erase(0, 3);
  }
}

bool load_markdown_template(const std::string& file_path, LoadedTemplate& out, std::string& error) {
  std::ifstream f(file_path);
  if (!f) {
    error = "无法打开模板: " + file_path;
    return false;
  }
  std::stringstream buf;
  buf << f.rdbuf();
  std::string all = buf.str();
  strip_utf8_bom(all);
  if (all.size() >= 4 && all.substr(0, 3) == "---") {
    size_t end = all.find("\n---", 3);
    if (end != std::string::npos) {
      std::string fm = all.substr(3, end - 3);
      out.body = all.substr(end + 4);
      strip_utf8_bom(out.body);
      std::istringstream ls(fm);
      std::string line;
      while (std::getline(ls, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string key = trim(line.substr(0, c));
        std::string val = trim(line.substr(c + 1));
        if (!val.empty() && (val.front() == '"' || val.front() == '\'')) val = val.substr(1);
        if (!val.empty() && (val.back() == '"' || val.back() == '\'')) val.pop_back();
        if (key == "name") out.name = val;
        if (key == "description") out.description = val;
      }
    } else {
      out.body = all;
    }
  } else {
    out.body = all;
  }
  if (out.name.empty()) {
    size_t slash = file_path.find_last_of("/\\");
    out.name = slash == std::string::npos ? file_path : file_path.substr(slash + 1);
  }
  return true;
}

}
