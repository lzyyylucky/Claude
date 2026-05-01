#include "file_chat_store.hpp"
#include "storage_helpers.hpp"

#include "../util/json_minimal.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace cct::storage {

namespace {

std::string safe_thread_stem(const std::string& tid) {
  std::string s;
  s.reserve(tid.size());
  for (char c : tid) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') s += c;
    else s += '_';
  }
  if (s.empty()) s = "t";
  return s;
}


void sanitize_thread_title_in_place(std::string& t) {
  for (char& c : t) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
}

/** threads.txt 为 TSV：锚点若含制表符/换行会破坏整行解析，进而丢会话列表。 */
void sanitize_thread_meta_tsv_field(std::string& s) {
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
}

static std::string messages_to_json_array(const std::vector<cct::llm::ChatMessage>& h) {
  std::ostringstream o;
  o << '[';
  for (size_t i = 0; i < h.size(); ++i) {
    if (i) o << ',';
    o << "{\"role\":\"" << cct::util::json_escape_string(h[i].role) << "\",\"content\":\""
      << cct::util::json_escape_string(h[i].content) << "\"}";
  }
  o << ']';
  return o.str();
}

}  // namespace

FileChatPersistence::FileChatPersistence(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {}

void FileChatPersistence::ensure_loaded(std::uint64_t uid) {
  if (hydrated_.count(uid)) return;
  const std::string uk = user_chats_key(uid);
  std::error_code ec;
  const std::filesystem::path dir = data_dir_ / "users" / std::to_string(uid) / "chats";
  const std::filesystem::path tf = dir / "threads.txt";
  if (!std::filesystem::is_regular_file(tf, ec)) {
    hydrated_.insert(uid);
    ensure_default_thread_list_vec(threads_[uk]);
    return;
  }
  std::ifstream in(tf, std::ios::binary);
  if (!in) {
    hydrated_.insert(uid);
    ensure_default_thread_list_vec(threads_[uk]);
    return;
  }
  std::vector<ChatThreadRow> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    std::string id = line.substr(0, t1);
    std::string title = line.substr(t1 + 1, t2 - t1 - 1);
    std::uint64_t up = 0;
    std::string anchor;
    try {
      const size_t t3 = line.find('\t', t2 + 1);
      if (t3 == std::string::npos) {
        up = static_cast<std::uint64_t>(std::stoull(line.substr(t2 + 1)));
      } else {
        up = static_cast<std::uint64_t>(std::stoull(line.substr(t2 + 1, t3 - t2 - 1)));
        anchor = line.substr(t3 + 1);
      }
    } catch (...) {}
    if (id.empty()) continue;
    sanitize_thread_meta_tsv_field(anchor);
    rows.push_back(ChatThreadRow{std::move(id), std::move(title), up, 0, std::move(anchor)});
  }
  if (rows.empty()) {
    hydrated_.insert(uid);
    ensure_default_thread_list_vec(threads_[uk]);
    return;
  }
  threads_[uk] = std::move(rows);
  {
    bool has_main = false;
    for (const auto& r : threads_[uk]) {
      if (r.id == "main") {
        has_main = true;
        break;
      }
    }
    if (!has_main) threads_[uk].emplace_back(ChatThreadRow{"main", "默认对话", 0, 0, ""});
  }
  for (const auto& row : threads_[uk]) {
    const std::string k = chat_hist_key(uk, row.id);
    const std::filesystem::path mf = dir / ("m_" + safe_thread_stem(row.id) + ".json");
    if (!std::filesystem::is_regular_file(mf, ec)) {
      history_[k] = {};
      continue;
    }
    std::ifstream mi(mf, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(mi)), std::istreambuf_iterator<char>());
    std::vector<std::pair<std::string, std::string>> tmp;
    std::string perr;
    if (!cct::util::json_parse_chat_messages_array(body, tmp, perr)) {
      history_[k] = {};
      continue;
    }
    std::vector<cct::llm::ChatMessage> mm;
    mm.reserve(tmp.size());
    for (auto& p : tmp) mm.push_back(cct::llm::ChatMessage{std::move(p.first), std::move(p.second)});
    history_[k] = std::move(mm);
  }
  hydrated_.insert(uid);
}

void FileChatPersistence::persist(std::uint64_t uid) {
  const std::string uk = user_chats_key(uid);
  std::error_code ec;
  const std::filesystem::path dir = data_dir_ / "users" / std::to_string(uid) / "chats";
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path tf = dir / "threads.txt";
  std::ofstream meta(tf, std::ios::binary | std::ios::trunc);
  if (!meta) return;
  const auto& rows = threads_[uk];
  for (const auto& row : rows) {
    std::string t = row.title;
    sanitize_thread_title_in_place(t);
    std::string a = row.workspace_anchor;
    sanitize_thread_meta_tsv_field(a);
    meta << row.id << '\t' << t << '\t' << row.updated << '\t' << a << '\n';
  }
  meta.close();
  for (const auto& row : rows) {
    const std::string k = chat_hist_key(uk, row.id);
    const std::string fn = "m_" + safe_thread_stem(row.id) + ".json";
    std::ofstream mf(dir / fn, std::ios::binary | std::ios::trunc);
    if (!mf) continue;
    auto it = history_.find(k);
    mf << (it == history_.end() ? "[]" : messages_to_json_array(it->second));
  }
}

}  // namespace cct::storage
