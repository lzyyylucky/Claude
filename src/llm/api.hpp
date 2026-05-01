#pragma once

#include <functional>
#include <string>
#include <vector>

namespace cct::util {
struct AppConfig;
}

namespace cct::llm {

struct LlmResult {
  bool ok = false;
  std::string text;
  std::string error;
  /** API usage；-1 表示响应未提供（智谱流式可能在末包给出） */
  int usage_prompt_tokens = -1;
  int usage_completion_tokens = -1;
  int usage_total_tokens = -1;
};

struct ChatMessage {
  std::string role;
  std::string content;
};

LlmResult call_mock(const std::string& user_prompt);
LlmResult call_anthropic(const cct::util::AppConfig& cfg, const std::string& user_prompt);

LlmResult call_mock_chat(const std::vector<ChatMessage>& messages);
LlmResult call_anthropic_chat(const cct::util::AppConfig& cfg, const std::vector<ChatMessage>& messages);

LlmResult call_zhipu(const cct::util::AppConfig& cfg, const std::string& user_prompt);
LlmResult call_zhipu_chat(const cct::util::AppConfig& cfg, const std::vector<ChatMessage>& messages);

/** 智谱流式：type 为 "c" 正文片段、"h" 思考片段；聚合写入 out_full_* */
bool call_zhipu_chat_stream(const cct::util::AppConfig& cfg, const std::vector<ChatMessage>& messages,
                            const std::function<bool(const std::string& type, const std::string& fragment)>& on_delta,
                            std::string& out_full_content, std::string& out_full_thinking, std::string& error,
                            int* usage_prompt_tokens = nullptr, int* usage_completion_tokens = nullptr,
                            int* usage_total_tokens = nullptr);

}
