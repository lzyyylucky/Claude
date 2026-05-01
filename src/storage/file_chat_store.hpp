#pragma once

#include "storage_iface.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace cct::storage {

class FileChatPersistence final : public IChatPersistence {
 public:
  explicit FileChatPersistence(std::filesystem::path data_dir);

  std::mutex& mutex() override { return mu_; }
  void ensure_loaded(std::uint64_t uid) override;
  void persist(std::uint64_t uid) override;
  std::unordered_map<std::string, std::vector<ChatThreadRow>>& threads_map() override { return threads_; }
  std::unordered_map<std::string, std::vector<cct::llm::ChatMessage>>& history_map() override { return history_; }
  std::unordered_set<std::uint64_t>& hydrated_users() override { return hydrated_; }

 private:
  std::filesystem::path data_dir_;
  std::mutex mu_;
  std::unordered_map<std::string, std::vector<ChatThreadRow>> threads_;
  std::unordered_map<std::string, std::vector<cct::llm::ChatMessage>> history_;
  std::unordered_set<std::uint64_t> hydrated_;
};

}  // namespace cct::storage
