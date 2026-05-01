#pragma once

#include "storage_iface.hpp"
#include "../web/user_store.hpp"

#include <filesystem>

namespace cct::storage {

/** 包装原有 JSONL UserStore，实现 IUserPersistence */
class FileUserPersistence final : public IUserPersistence {
 public:
  explicit FileUserPersistence(std::filesystem::path users_path) : store_(std::move(users_path)) {}

  bool register_user(const std::string& username, const std::string& password, std::uint64_t& out_id,
                     std::string& error) override {
    return store_.register_user(username, password, out_id, error);
  }
  bool verify_login(const std::string& username, const std::string& password, std::uint64_t& out_id,
                    std::string& error) override {
    return store_.verify_login(username, password, out_id, error);
  }
  bool get_user_by_id(std::uint64_t id, UserRowLite& out, std::string& error) override {
    cct::web::UserRow ur;
    if (!store_.get_user_by_id(id, ur, error)) return false;
    out.id = ur.id;
    out.username = std::move(ur.username);
    out.display_name = std::move(ur.display_name);
    out.salt_hex = std::move(ur.salt_hex);
    out.hash_hex = std::move(ur.hash_hex);
    return true;
  }
  bool update_display_name(std::uint64_t id, const std::string& display_name, std::string& error) override {
    return store_.update_display_name(id, display_name, error);
  }
  bool get_ui_theme(std::uint64_t id, std::string& theme_out, std::string& error) override {
    return store_.get_ui_theme(id, theme_out, error);
  }
  bool set_ui_theme(std::uint64_t id, const std::string& theme, std::string& error) override {
    return store_.set_ui_theme(id, theme, error);
  }

 private:
  cct::web::UserStore store_;
};

}  // namespace cct::storage
