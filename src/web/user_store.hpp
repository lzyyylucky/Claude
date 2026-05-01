#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cct::web {

struct UserRow {
  std::uint64_t id = 0;
  std::string username;
  /** 展示名；空表示界面回退为 username */
  std::string display_name;
  std::string salt_hex;
  std::string hash_hex;
};

class UserStore {
 public:
  explicit UserStore(std::filesystem::path path) : path_(std::move(path)) {}

  bool register_user(const std::string& username, const std::string& password, std::uint64_t& out_id,
                     std::string& error);
  bool verify_login(const std::string& username, const std::string& password, std::uint64_t& out_id,
                    std::string& error);

  bool get_user_by_id(std::uint64_t id, UserRow& out, std::string& error);
  bool update_display_name(std::uint64_t id, const std::string& display_name, std::string& error);

  /** 用户子目录内 preferences.json，字段 theme */
  bool get_ui_theme(std::uint64_t id, std::string& theme_out, std::string& error);
  bool set_ui_theme(std::uint64_t id, const std::string& theme, std::string& error);

 private:
  std::filesystem::path path_;
  mutable std::mutex mu_;
  std::vector<UserRow> users_;
  std::uint64_t next_id_ = 1;
};

}
