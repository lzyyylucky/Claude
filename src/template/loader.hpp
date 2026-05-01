#pragma once

#include <string>

namespace cct::template_ns {

struct LoadedTemplate {
  std::string name;
  std::string description;
  std::string body;
};

bool load_markdown_template(const std::string& file_path, LoadedTemplate& out, std::string& error);

}
