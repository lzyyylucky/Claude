#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cct::context {

struct ScanOptions {
  std::filesystem::path root;
  int max_total_chars = 12000;
  std::vector<std::string> extra_ignore_dir_names;
};

std::string scan_project_context(const ScanOptions& opt, std::string& error);

}
