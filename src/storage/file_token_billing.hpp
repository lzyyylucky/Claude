#pragma once

#include "storage_iface.hpp"
#include "token_billing_common.hpp"

#include <filesystem>
#include <mutex>

namespace cct::storage {

class FileTokenBillingPersistence final : public ITokenBillingPersistence {
 public:
  explicit FileTokenBillingPersistence(std::filesystem::path data_dir_root);

  bool get_state(std::uint64_t user_id, TokenBillingState& out, std::string& error) override;
  bool check_can_use(std::uint64_t user_id, std::int64_t min_tokens_needed, TokenBillingState& state_out,
                     std::string& error) override;
  bool add_consumed(std::uint64_t user_id, std::int64_t delta, TokenBillingState& state_out,
                    std::string& error) override;
  bool apply_subscription(std::uint64_t user_id, const std::string& tier, const std::string& pay_method,
                          std::string& out_txn_id, std::string& error) override;

 private:
  std::filesystem::path billing_path(std::uint64_t user_id) const;
  bool load_or_init_unlocked(std::uint64_t user_id, TokenBillingState& out, std::string& error);
  bool save_unlocked(const std::filesystem::path& path, const TokenBillingState& s);

  std::filesystem::path data_dir_;
  mutable std::mutex mu_;
};

}  // namespace cct::storage
