#pragma once

#include "../llm/api.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cct::storage {

struct ChatThreadRow {
  std::string id;
  std::string title;
  std::uint64_t updated = 0;
  /** 仅 SQL 后端使用；文件后端保持 0 */
  std::uint64_t ordinal = 0;
  /** 相对 workspace_root 的会话项目目录锚点，例如 calculate */
  std::string workspace_anchor;
};

/** 与原先 server.cpp 内存结构一致：便于文件/SQL 两套后端共用 handle_api 逻辑 */
class IChatPersistence {
 public:
  virtual ~IChatPersistence() = default;
  virtual std::mutex& mutex() = 0;
  virtual void ensure_loaded(std::uint64_t uid) = 0;
  virtual void persist(std::uint64_t uid) = 0;
  virtual std::unordered_map<std::string, std::vector<ChatThreadRow>>& threads_map() = 0;
  virtual std::unordered_map<std::string, std::vector<cct::llm::ChatMessage>>& history_map() = 0;
  virtual std::unordered_set<std::uint64_t>& hydrated_users() = 0;
};

struct UserRowLite {
  std::uint64_t id = 0;
  std::string username;
  std::string display_name;
  std::string salt_hex;
  std::string hash_hex;
};

class IUserPersistence {
 public:
  virtual ~IUserPersistence() = default;
  virtual bool register_user(const std::string& username, const std::string& password, std::uint64_t& out_id,
                             std::string& error) = 0;
  virtual bool verify_login(const std::string& username, const std::string& password, std::uint64_t& out_id,
                            std::string& error) = 0;
  virtual bool get_user_by_id(std::uint64_t id, UserRowLite& out, std::string& error) = 0;
  virtual bool update_display_name(std::uint64_t id, const std::string& display_name, std::string& error) = 0;
  /** UI 主题偏好：dark / light / system，存于 UserPreferences（SQL）或用户目录 preferences.json（文件） */
  virtual bool get_ui_theme(std::uint64_t id, std::string& theme_out, std::string& error) = 0;
  virtual bool set_ui_theme(std::uint64_t id, const std::string& theme, std::string& error) = 0;
};

class IComponentPersistence {
 public:
  virtual ~IComponentPersistence() = default;
  virtual void ensure_default_samples(std::uint64_t uid) = 0;
  virtual bool list_stems(std::uint64_t uid, const char* cat, std::vector<std::string>& stems) = 0;
  virtual bool get_content(std::uint64_t uid, const char* cat, const std::string& name, std::string& body,
                           std::string& error) = 0;
  virtual bool create_new(std::uint64_t uid, const char* cat, const std::string& name, const std::string& content,
                          std::string& error) = 0;
  virtual bool update_content(std::uint64_t uid, const char* cat, const std::string& name, const std::string& content,
                              std::string& error) = 0;
  virtual bool remove(std::uint64_t uid, const char* cat, const std::string& name, std::string& error) = 0;
};

/** 订阅档位与按月 Token 配额（自然月滚动消耗计数） */
struct TokenBillingState {
  std::string tier;
  std::int64_t token_quota = 0;
  std::int64_t tokens_consumed = 0;
  int period_yyyymm = 0;
};

class ITokenBillingPersistence {
 public:
  virtual ~ITokenBillingPersistence() = default;
  /** 若不存在则写入默认 free；若月份变更则清零 consumed 并按档位刷新 quota */
  virtual bool get_state(std::uint64_t user_id, TokenBillingState& out, std::string& error) = 0;
  /** 若剩余额度不足以覆盖 min_tokens_needed 则失败 */
  virtual bool check_can_use(std::uint64_t user_id, std::int64_t min_tokens_needed, TokenBillingState& state_out,
                             std::string& error) = 0;
  virtual bool add_consumed(std::uint64_t user_id, std::int64_t delta, TokenBillingState& state_out,
                            std::string& error) = 0;
  /** 模拟开通：更新 tier/quota，consumed 归零，周期对齐当月 */
  virtual bool apply_subscription(std::uint64_t user_id, const std::string& tier, const std::string& pay_method,
                                  std::string& out_txn_id, std::string& error) = 0;
};

inline std::string user_chats_key(std::uint64_t uid) { return std::string("u") + std::to_string(uid); }

inline std::string chat_hist_key(const std::string& user_key, const std::string& thread_id) {
  return user_key + std::string("\x1e") + thread_id;
}

}  // namespace cct::storage
