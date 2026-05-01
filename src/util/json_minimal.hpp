#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cct::util {

std::string json_escape_string(const std::string& s);

bool json_extract_string_after_key(const std::string& json, const std::string& key, std::string& out);

/** 仅解析 [ {"role":"...","content":"..."}, ... ] 形式的数组（供聊天历史持久化） */
bool json_parse_chat_messages_array(const std::string& json, std::vector<std::pair<std::string, std::string>>& out,
                                    std::string& error);

bool anthropic_extract_first_text(const std::string& response_body, std::string& out_text, std::string& error);

/** 智谱 / OpenAI 兼容 chat.completions 响应中第一条 assistant 文本 */
bool openai_style_extract_assistant_content(const std::string& response_body, std::string& out_text,
                                            std::string& error);

/** 从单次 HTTP JSON 或 SSE data 片段中提取 token usage（支持 prompt_tokens / Anthropic input_tokens 等） */
bool json_try_extract_llm_usage_tokens(const std::string& body, int& prompt_tokens, int& completion_tokens,
                                       int& total_tokens);

}  // namespace cct::util
