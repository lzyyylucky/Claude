#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cct::generator {

struct ApplyResult {
  int files_written = 0;
  std::vector<std::string> messages;
};

ApplyResult apply_model_output(const std::string& model_text, const std::filesystem::path& base_out,
                               bool dry_run, bool force);

}
