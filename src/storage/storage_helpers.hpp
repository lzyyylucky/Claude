#pragma once

#include "storage_iface.hpp"

#include "../llm/api.hpp"
#include "../util/utf8_string.hpp"

#include <string>
#include <vector>

namespace cct::storage {

inline void ensure_default_thread_list_vec(std::vector<ChatThreadRow>& v) {
  if (v.empty()) v.push_back(ChatThreadRow{"main", "默认对话", 0, 0});
}

inline bool is_placeholder_thread_title(const std::string& t) {
  return t.empty() || t == "新对话" || t == "默认对话";
}

inline std::string thread_title_from_history_or_incoming(const std::vector<cct::llm::ChatMessage>& hist_before_push,
                                                       const std::string& incoming_user_msg) {
  for (const auto& m : hist_before_push) {
    if (m.role == "user" && !m.content.empty()) return m.content;
  }
  return incoming_user_msg;
}

inline void apply_thread_title_from_first_user(std::vector<ChatThreadRow>& rows, const std::string& thread_id,
                                               const std::string& incoming_user_msg,
                                               const std::vector<cct::llm::ChatMessage>& hist_before_push) {
  for (auto& row : rows) {
    if (row.id != thread_id || !is_placeholder_thread_title(row.title)) continue;
    std::string raw = thread_title_from_history_or_incoming(hist_before_push, incoming_user_msg);
    /** 按字符截断（勿用 substr 字节数），避免 UTF-8 中英混排时出现 � */
    row.title = cct::util::utf8_ellipsis_prefix_chars(raw, 18);
    row.updated++;
    break;
  }
}

}  // namespace cct::storage
