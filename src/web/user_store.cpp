#include "user_store.hpp"

#include "../util/json_minimal.hpp"
#include "crypto.hpp"

#include <fstream>
#include <sstream>

namespace cct::web {

namespace {

bool parse_user_line(const std::string& line, UserRow& row) {
  std::string id_s, user, salt, hash;
  if (!cct::util::json_extract_string_after_key(line, "id", id_s)) return false;
  if (!cct::util::json_extract_string_after_key(line, "username", user)) return false;
  if (!cct::util::json_extract_string_after_key(line, "salt_hex", salt)) return false;
  if (!cct::util::json_extract_string_after_key(line, "hash_hex", hash)) return false;
  try {
    row.id = static_cast<std::uint64_t>(std::stoull(id_s));
  } catch (...) {
    return false;
  }
  row.username = std::move(user);
  row.salt_hex = std::move(salt);
  row.hash_hex = std::move(hash);
  row.display_name.clear();
  cct::util::json_extract_string_after_key(line, "display_name", row.display_name);
  return true;
}

void write_user_line(std::ostream& out, const UserRow& r) {
  out << "{\"id\":\"" << r.id << "\",\"username\":\"" << cct::util::json_escape_string(r.username)
      << "\",\"display_name\":\"" << cct::util::json_escape_string(r.display_name) << "\",\"salt_hex\":\""
      << r.salt_hex << "\",\"hash_hex\":\"" << r.hash_hex << "\"}\n";
}

std::filesystem::path user_prefs_json_path(const std::filesystem::path& users_jsonl, std::uint64_t id) {
  return users_jsonl.parent_path() / "users" / std::to_string(id) / "preferences.json";
}

bool normalize_ui_theme_str(const std::string& t, std::string& out) {
  std::string s = t;
  for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  if (s == "dark" || s == "light" || s == "system") {
    out = std::move(s);
    return true;
  }
  return false;
}

bool user_id_exists_in_store_file(const std::filesystem::path& path, std::uint64_t id) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    UserRow row;
    if (!parse_user_line(line, row)) continue;
    if (row.id == id) return true;
  }
  return false;
}

}  // namespace

bool UserStore::get_user_by_id(std::uint64_t id, UserRow& out, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    error = "用户不存在";
    return false;
  }
  std::ifstream in(path_, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    UserRow row;
    if (!parse_user_line(line, row)) continue;
    if (row.id == id) {
      out = std::move(row);
      return true;
    }
  }
  error = "用户不存在";
  return false;
}

bool UserStore::update_display_name(std::uint64_t id, const std::string& display_name, std::string& error) {
  std::string trimmed = display_name;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
  if (trimmed.empty()) {
    error = "显示名称不能为空";
    return false;
  }
  if (trimmed.size() > 64) {
    error = "显示名称过长（最多 64 字符）";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<UserRow> rows;
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    error = "数据不存在";
    return false;
  }
  std::ifstream in(path_, std::ios::binary);
  std::string line;
  bool found = false;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    UserRow row;
    if (!parse_user_line(line, row)) continue;
    if (row.id == id) {
      row.display_name = trimmed;
      found = true;
    }
    rows.push_back(std::move(row));
  }
  if (!found) {
    error = "用户不存在";
    return false;
  }
  std::filesystem::path dir = path_.parent_path();
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  for (const auto& r : rows) write_user_line(out, r);
  return true;
}

bool UserStore::register_user(const std::string& username, const std::string& password, std::uint64_t& out_id,
                              std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  if (std::filesystem::exists(path_, ec)) {
    std::ifstream in(path_, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      UserRow row;
      if (!parse_user_line(line, row)) continue;
      users_.push_back(std::move(row));
      if (users_.back().id >= next_id_) next_id_ = users_.back().id + 1;
    }
  }
  for (const auto& r : users_) {
    if (r.username == username) {
      error = "用户名已存在";
      return false;
    }
  }
  std::vector<unsigned char> salt;
  if (!crypto::random_bytes(salt, 16)) {
    error = "随机数失败";
    return false;
  }
  std::vector<unsigned char> hash;
  if (!crypto::pbkdf2_sha256(password, salt, hash)) {
    error = "哈希失败";
    return false;
  }
  UserRow row;
  row.id = next_id_++;
  row.username = username;
  row.display_name = username;
  row.salt_hex = crypto::bytes_to_hex(salt.data(), salt.size());
  row.hash_hex = crypto::bytes_to_hex(hash.data(), hash.size());
  users_.push_back(row);
  out_id = row.id;
  std::filesystem::path dir = path_.parent_path();
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  for (const auto& r : users_) write_user_line(out, r);
  return true;
}

bool UserStore::verify_login(const std::string& username, const std::string& password, std::uint64_t& out_id,
                             std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    error = "用户名或密码错误";
    return false;
  }
  std::ifstream in(path_, std::ios::binary);
  std::string line;
  UserRow found_row{};
  bool have = false;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    UserRow row;
    if (!parse_user_line(line, row)) continue;
    if (row.username == username) {
      found_row = std::move(row);
      have = true;
      break;
    }
  }
  if (!have) {
    error = "用户名或密码错误";
    return false;
  }
  std::vector<unsigned char> salt, expect;
  if (!crypto::hex_to_bytes(found_row.salt_hex, salt) || !crypto::hex_to_bytes(found_row.hash_hex, expect)) {
    error = "用户数据损坏";
    return false;
  }
  std::vector<unsigned char> got;
  if (!crypto::pbkdf2_sha256(password, salt, got)) {
    error = "哈希失败";
    return false;
  }
  std::string got_hex = crypto::bytes_to_hex(got.data(), got.size());
  if (!crypto::timing_equal_hex(got_hex, found_row.hash_hex)) {
    error = "用户名或密码错误";
    return false;
  }
  out_id = found_row.id;
  return true;
}

bool UserStore::get_ui_theme(std::uint64_t id, std::string& theme_out, std::string& error) {
  (void)error;
  theme_out = "dark";
  const auto p = user_prefs_json_path(path_, id);
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return true;
  std::ifstream in(p, std::ios::binary);
  if (!in) return true;
  std::stringstream buf;
  buf << in.rdbuf();
  std::string t;
  if (cct::util::json_extract_string_after_key(buf.str(), "theme", t)) {
    std::string norm;
    if (normalize_ui_theme_str(t, norm)) theme_out = std::move(norm);
  }
  return true;
}

bool UserStore::set_ui_theme(std::uint64_t id, const std::string& theme, std::string& error) {
  std::string norm;
  if (!normalize_ui_theme_str(theme, norm)) {
    error = "theme 须为 dark、light 或 system";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    error = "数据不存在";
    return false;
  }
  if (!user_id_exists_in_store_file(path_, id)) {
    error = "用户不存在";
    return false;
  }
  const auto p = user_prefs_json_path(path_, id);
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "无法写入偏好";
    return false;
  }
  out << "{\"theme\":\"" << norm << "\"}\n";
  return true;
}

}  // namespace cct::web
