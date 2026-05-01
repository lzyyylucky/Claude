#include "file_token_billing.hpp"

#include "../util/json_minimal.hpp"
#include "../web/crypto.hpp"

#include <fstream>
#include <regex>
#include <sstream>

namespace cct::storage {

namespace {

bool re_i64_json(const std::string& j, const char* key, std::int64_t& out) {
  std::string pat = std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+)";
  std::regex re(pat);
  std::smatch m;
  if (!std::regex_search(j, m, re)) return false;
  try {
    out = static_cast<std::int64_t>(std::stoll(m[1].str()));
  } catch (...) {
    return false;
  }
  return true;
}

bool re_int_json(const std::string& j, const char* key, int& out) {
  std::string pat = std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+)";
  std::regex re(pat);
  std::smatch m;
  if (!std::regex_search(j, m, re)) return false;
  try {
    out = std::stoi(m[1].str());
  } catch (...) {
    return false;
  }
  return true;
}

void apply_period_rollover(TokenBillingState& s) {
  const int nowp = billing_current_period_yyyymm();
  if (s.period_yyyymm != nowp) {
    s.period_yyyymm = nowp;
    s.tokens_consumed = 0;
    s.token_quota = billing_token_quota_for_tier(billing_normalize_tier(s.tier));
  }
}

}  // namespace

FileTokenBillingPersistence::FileTokenBillingPersistence(std::filesystem::path data_dir_root)
    : data_dir_(std::move(data_dir_root)) {}

std::filesystem::path FileTokenBillingPersistence::billing_path(std::uint64_t user_id) const {
  return data_dir_ / "users" / std::to_string(user_id) / "billing.json";
}

bool FileTokenBillingPersistence::save_unlocked(const std::filesystem::path& path, const TokenBillingState& s) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "{\"tier\":\"" << cct::util::json_escape_string(s.tier) << "\",\"token_quota\":" << s.token_quota
      << ",\"tokens_consumed\":" << s.tokens_consumed << ",\"period_yyyymm\":" << s.period_yyyymm << "}\n";
  return true;
}

bool FileTokenBillingPersistence::load_or_init_unlocked(std::uint64_t user_id, TokenBillingState& out,
                                                        std::string& error) {
  const auto path = billing_path(user_id);
  TokenBillingState s{};
  std::ifstream in(path, std::ios::binary);
  if (in) {
    std::stringstream buf;
    buf << in.rdbuf();
    std::string j = buf.str();
    std::string tier_raw;
    if (cct::util::json_extract_string_after_key(j, "tier", tier_raw)) s.tier = billing_normalize_tier(tier_raw);
    std::int64_t q = 0, c = 0;
    if (re_i64_json(j, "token_quota", q)) s.token_quota = q;
    if (re_i64_json(j, "tokens_consumed", c)) s.tokens_consumed = c;
    int p = 0;
    if (re_int_json(j, "period_yyyymm", p)) s.period_yyyymm = p;
    if (s.tier.empty()) s.tier = "free";
    s.tier = billing_normalize_tier(s.tier);
    if (s.token_quota <= 0) s.token_quota = billing_token_quota_for_tier(s.tier);
    if (s.period_yyyymm <= 0) s.period_yyyymm = billing_current_period_yyyymm();
    apply_period_rollover(s);
    /** 滚动后若 quota 与档位不一致（例如手动改过档），以档位为准 */
    s.token_quota = billing_token_quota_for_tier(s.tier);
    out = s;
    if (!save_unlocked(path, s)) {
      error = "写入计费数据失败";
      return false;
    }
    return true;
  }
  s.tier = "free";
  s.token_quota = billing_token_quota_for_tier(s.tier);
  s.tokens_consumed = 0;
  s.period_yyyymm = billing_current_period_yyyymm();
  out = s;
  if (!save_unlocked(path, s)) {
    error = "初始化计费数据失败";
    return false;
  }
  return true;
}

bool FileTokenBillingPersistence::get_state(std::uint64_t user_id, TokenBillingState& out, std::string& error) {
  std::lock_guard<std::mutex> lk(mu_);
  return load_or_init_unlocked(user_id, out, error);
}

bool FileTokenBillingPersistence::check_can_use(std::uint64_t user_id, std::int64_t min_tokens_needed,
                                                  TokenBillingState& state_out, std::string& error) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!load_or_init_unlocked(user_id, state_out, error)) return false;
  const std::int64_t rem = state_out.token_quota - state_out.tokens_consumed;
  if (min_tokens_needed > rem) {
    error = "本月 Token 额度已用尽，请升级订阅后继续。";
    return false;
  }
  return true;
}

bool FileTokenBillingPersistence::add_consumed(std::uint64_t user_id, std::int64_t delta, TokenBillingState& state_out,
                                               std::string& error) {
  if (delta <= 0) {
    std::lock_guard<std::mutex> lk(mu_);
    return load_or_init_unlocked(user_id, state_out, error);
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (!load_or_init_unlocked(user_id, state_out, error)) return false;
  state_out.tokens_consumed += delta;
  if (!save_unlocked(billing_path(user_id), state_out)) {
    error = "更新消耗计数失败";
    return false;
  }
  return true;
}

bool FileTokenBillingPersistence::apply_subscription(std::uint64_t user_id, const std::string& tier,
                                                     const std::string& pay_method, std::string& out_txn_id,
                                                     std::string& error) {
  std::string norm = billing_normalize_tier(tier);
  if (!billing_tier_is_paid(norm)) {
    error = "无效套餐";
    return false;
  }
  std::string pm = pay_method;
  for (char& c : pm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (pm != "wechat" && pm != "alipay") {
    error = "支付方式须为 wechat 或 alipay";
    return false;
  }
  std::vector<unsigned char> rb;
  if (!cct::web::crypto::random_bytes(rb, 8)) {
    error = "随机数失败";
    return false;
  }
  out_txn_id = std::string("mock-") + pm + "-" + cct::web::crypto::bytes_to_hex(rb.data(), rb.size());

  std::lock_guard<std::mutex> lk(mu_);
  TokenBillingState s{};
  if (!load_or_init_unlocked(user_id, s, error)) return false;
  s.tier = norm;
  s.token_quota = billing_token_quota_for_tier(norm);
  s.tokens_consumed = 0;
  s.period_yyyymm = billing_current_period_yyyymm();
  if (!save_unlocked(billing_path(user_id), s)) {
    error = "写入订阅失败";
    return false;
  }
  return true;
}

}  // namespace cct::storage
