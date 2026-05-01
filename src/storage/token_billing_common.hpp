#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <string>

namespace cct::storage {

inline int billing_current_period_yyyymm() {
  std::time_t t = std::time(nullptr);
  std::tm lt{};
#if defined(_WIN32)
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif
  return (lt.tm_year + 1900) * 100 + (lt.tm_mon + 1);
}

inline std::string billing_normalize_tier(std::string t) {
  for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "go" || t == "plus" || t == "pro" || t == "free") return t;
  return "free";
}

inline std::int64_t billing_token_quota_for_tier(const std::string& tier_norm) {
  if (tier_norm == "go") return 300000;
  if (tier_norm == "plus") return 1200000;
  if (tier_norm == "pro") return 6000000;
  return 50000;
}

/** 各付费档单日模型调用（对话+检测）上限；免费档由配置项单独处理 */
inline int billing_daily_llm_calls_cap_for_tier(const std::string& tier_norm) {
  if (tier_norm == "go") return 120;
  if (tier_norm == "plus") return 600;
  if (tier_norm == "pro") return 2500;
  return 0;
}

/**
 * 返回单日模型调用上限：0 表示不限。
 * 免费档：llm_daily_call_limit>0 时用配置值；配置为 0 时不限（与旧版一致）。
 * 付费档：使用档位内置上限。
 */
inline int billing_effective_daily_llm_calls(const std::string& tier_norm, int cfg_llm_daily_call_limit) {
  if (tier_norm != "free") return billing_daily_llm_calls_cap_for_tier(tier_norm);
  if (cfg_llm_daily_call_limit > 0) return cfg_llm_daily_call_limit;
  return 0;
}

inline std::string billing_subscription_label_zh(const std::string& tier_norm) {
  if (tier_norm == "go") return "Go";
  if (tier_norm == "plus") return "Plus";
  if (tier_norm == "pro") return "Pro";
  return "免费版";
}

inline bool billing_tier_is_paid(const std::string& tier_norm) {
  return tier_norm == "go" || tier_norm == "plus" || tier_norm == "pro";
}

}  // namespace cct::storage
