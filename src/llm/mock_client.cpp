#include "api.hpp"

#include <sstream>

namespace cct::llm {

LlmResult call_mock(const std::string& user_prompt) {
  LlmResult r;
  r.ok = true;
  r.usage_prompt_tokens = 0;
  r.usage_completion_tokens = 0;
  r.usage_total_tokens = 0;
  std::ostringstream o;
  o << "以下是 Mock 模式下的示例输出（未调用真实 API）。\n\n";
  o << "用户需求摘要：\n" << user_prompt << "\n\n";
  o << "```generated/mock_hello.cpp\n";
  o << "#include <iostream>\n";
  o << "int main() {\n";
  o << "  std::cout << \"欢迎使用 cct-cn（毕设版）\\n\";\n";
  o << "  return 0;\n";
  o << "}\n";
  o << "```\n";
  r.text = o.str();
  return r;
}

static std::string last_user_snippet(const std::vector<ChatMessage>& messages) {
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (it->role == "user") {
      std::string s = it->content;
      if (s.size() > 200) s.resize(200);
      return s;
    }
  }
  return std::string();
}

LlmResult call_mock_chat(const std::vector<ChatMessage>& messages) {
  std::string last = last_user_snippet(messages);
  LlmResult r = call_mock(last);
  if (last.find("CCT应用测试") != std::string::npos) {
    r.text +=
        "\n\nCCT_APPLY:\n"
        "{\"path\":\"demo_cct.txt\",\"content\":\"// Mock CCT_APPLY 写入测试\\n// 发送「CCT应用测试」触发\\n\"}";
  }
  return r;
}

}
