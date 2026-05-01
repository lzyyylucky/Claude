#pragma once

#include "storage_iface.hpp"

#include <filesystem>

namespace cct::storage {

class FileComponentPersistence final : public IComponentPersistence {
 public:
  explicit FileComponentPersistence(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {}

  void ensure_default_samples(std::uint64_t uid) override;
  bool list_stems(std::uint64_t uid, const char* cat, std::vector<std::string>& stems) override;
  bool get_content(std::uint64_t uid, const char* cat, const std::string& name, std::string& body,
                   std::string& error) override;
  bool create_new(std::uint64_t uid, const char* cat, const std::string& name, const std::string& content,
                  std::string& error) override;
  bool update_content(std::uint64_t uid, const char* cat, const std::string& name, const std::string& content,
                      std::string& error) override;
  bool remove(std::uint64_t uid, const char* cat, const std::string& name, std::string& error) override;

 private:
  std::filesystem::path data_dir_;
  std::filesystem::path base_for(std::uint64_t uid, const char* cat) const {
    return data_dir_ / "users" / std::to_string(uid) / "components" / cat;
  }
};

}  // namespace cct::storage
