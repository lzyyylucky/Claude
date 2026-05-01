#include "json_minimal.hpp"

#include <cctype>
#include <climits>
#include <cstdio>

namespace cct::util {

std::string json_escape_string(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\':
        o += "\\\\";
        break;
      case '"':
        o += "\\\"";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o;
}

static void append_utf8(std::string& o, unsigned int cp) {
  if (cp < 0x80) {
    o += static_cast<char>(cp);
  } else if (cp < 0x800) {
    o += static_cast<char>(0xc0 | (cp >> 6));
    o += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    o += static_cast<char>(0xe0 | (cp >> 12));
    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    o += static_cast<char>(0x80 | (cp & 0x3f));
  }
}

static bool read_json_string_value(const std::string& s, size_t start_quote, std::string& out, size_t& end_pos,
                                   std::string& error) {
  if (start_quote >= s.size() || s[start_quote] != '"') {
    error = "内部解析错误：期望双引号字符串";
    return false;
  }
  size_t i = start_quote + 1;
  out.clear();
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"') {
      end_pos = i + 1;
      return true;
    }
    if (c == '\\' && i + 1 < s.size()) {
      char n = s[i + 1];
      if (n == 'n') {
        out += '\n';
        i += 2;
      } else if (n == 'r') {
        out += '\r';
        i += 2;
      } else if (n == 't') {
        out += '\t';
        i += 2;
      } else if (n == '\\' || n == '"') {
        out += n;
        i += 2;
      } else if (n == 'u' && i + 5 < s.size()) {
        unsigned int cp = 0;
        if (std::sscanf(s.c_str() + static_cast<int>(i) + 2, "%4x", &cp) == 1) {
          append_utf8(out, cp);
          i += 6;
        } else {
          out += n;
          i += 2;
        }
      } else {
        out += n;
        i += 2;
      }
    } else {
      out += static_cast<char>(c);
      ++i;
    }
  }
  error = "JSON 字符串未闭合";
  return false;
}

static bool slice_is_type_text(const std::string& body, size_t type_pos) {
  size_t colon = body.find(':', type_pos);
  if (colon == std::string::npos) return false;
  size_t q1 = body.find('"', colon);
  if (q1 == std::string::npos) return false;
  std::string v;
  size_t end = 0;
  std::string err;
  if (!read_json_string_value(body, q1, v, end, err)) return false;
  return v == "text";
}

bool json_extract_string_after_key(const std::string& json, const std::string& key, std::string& out) {
  if (key.empty()) return false;
  /** 不用 std::regex：workspace_bundle / editor_content 可达数百 KB，regex 在 MSVC 上会栈溢出 (0xC00000FD)。 */
  const std::string needle = std::string("\"") + key + "\"";
  size_t search = 0;
  while (search < json.size()) {
    const size_t kpos = json.find(needle, search);
    if (kpos == std::string::npos) return false;
    size_t i = kpos + needle.size();
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != ':') {
      search = kpos + 1;
      continue;
    }
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != '"') {
      search = kpos + 1;
      continue;
    }
    size_t endp = 0;
    std::string err;
    if (read_json_string_value(json, i, out, endp, err)) return true;
    search = kpos + 1;
  }
  return false;
}

bool anthropic_extract_first_text(const std::string& response_body, std::string& out_text, std::string& error) {
  size_t from = 0;
  for (;;) {
    size_t type_pos = response_body.find("\"type\"", from);
    if (type_pos == std::string::npos) break;
    if (!slice_is_type_text(response_body, type_pos)) {
      from = type_pos + 6;
      continue;
    }
    size_t text_key = response_body.find("\"text\"", type_pos);
    if (text_key == std::string::npos) {
      from = type_pos + 6;
      continue;
    }
    size_t colon = response_body.find(':', text_key + 6);
    if (colon == std::string::npos) {
      from = type_pos + 6;
      continue;
    }
    size_t q = colon + 1;
    while (q < response_body.size() && (response_body[q] == ' ' || response_body[q] == '\t')) ++q;
    size_t endp = 0;
    if (read_json_string_value(response_body, q, out_text, endp, error)) return true;
    from = type_pos + 6;
  }
  if (response_body.find("\"error\"") != std::string::npos) {
    std::string msg;
    if (json_extract_string_after_key(response_body, "message", msg)) {
      error = "API 错误: " + msg;
    } else {
      error = "API 返回 error 字段，完整响应片段: " + response_body.substr(0, 400);
    }
    return false;
  }
  error = "未解析到 type 为 text 的 content 块";
  return false;
}

bool openai_style_extract_assistant_content(const std::string& body, std::string& out_text, std::string& error) {
  size_t errk = body.find("\"error\"");
  if (errk != std::string::npos) {
    size_t brace = body.find('{', errk);
    if (brace != std::string::npos) {
      size_t span_end = std::min(body.size(), brace + 2000);
      std::string frag = body.substr(brace, span_end - brace);
      std::string msg;
      if (json_extract_string_after_key(frag, "message", msg)) {
        error = msg;
        return false;
      }
    }
    error = "API 返回 error 字段";
    return false;
  }

  size_t ch = body.find("\"choices\"");
  if (ch == std::string::npos) {
    error = "响应中无 choices 字段";
    return false;
  }
  size_t msg = body.find("\"message\"", ch);
  if (msg == std::string::npos) {
    error = "choices 中无 message";
    return false;
  }
  size_t ck = body.find("\"content\"", msg);
  if (ck == std::string::npos) {
    error = "message 中无 content";
    return false;
  }
  size_t colon = body.find(':', ck + 9);
  if (colon == std::string::npos) {
    error = "content 后无冒号";
    return false;
  }
  size_t q = colon + 1;
  while (q < body.size() &&
         (body[q] == ' ' || body[q] == '\t' || body[q] == '\n' || body[q] == '\r'))
    ++q;
  if (q + 4 <= body.size() && body[q] == 'n' && body.compare(q, 4, "null") == 0) {
    out_text.clear();
    return true;
  }
  if (q >= body.size() || body[q] != '"') {
    error = "content 非 JSON 字符串（可能为多模态结构），片段: " + body.substr(ck, std::min<std::size_t>(160, body.size() - ck));
    return false;
  }
  size_t endp = 0;
  return read_json_string_value(body, q, out_text, endp, error);
}

bool json_parse_chat_messages_array(const std::string& json, std::vector<std::pair<std::string, std::string>>& out,
                                    std::string& error) {
  out.clear();
  size_t i = 0;
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
  if (i >= json.size() || json[i] != '[') {
    error = "expected [";
    return false;
  }
  ++i;
  for (;;) {
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i < json.size() && json[i] == ']') return true;
    if (i >= json.size() || json[i] != '{') {
      error = "expected { or ]";
      return false;
    }
    const size_t obj_start = i;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (; i < json.size(); ++i) {
      if (in_str) {
        if (esc) {
          esc = false;
        } else if (json[i] == '\\') {
          esc = true;
        } else if (json[i] == '"') {
          in_str = false;
        }
        continue;
      }
      if (json[i] == '"') {
        in_str = true;
        continue;
      }
      if (json[i] == '{') ++depth;
      if (json[i] == '}') {
        --depth;
        if (depth == 0) {
          ++i;
          break;
        }
      }
    }
    if (depth != 0) {
      error = "unclosed object in array";
      return false;
    }
    const std::string obj = json.substr(obj_start, i - obj_start);
    std::string role, content;
    if (!json_extract_string_after_key(obj, "role", role) ||
        !json_extract_string_after_key(obj, "content", content)) {
      error = "message object needs role and content";
      return false;
    }
    out.emplace_back(std::move(role), std::move(content));
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i < json.size() && json[i] == ',') {
      ++i;
      continue;
    }
    if (i < json.size() && json[i] == ']') return true;
    error = "expected , or ] after object";
    return false;
  }
}

static bool json_first_nonnegative_int_after_key(const std::string& j, const std::string& key, int& out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = 0;
  while ((pos = j.find(needle, pos)) != std::string::npos) {
    size_t i = pos + needle.size();
    while (i < j.size() && std::isspace(static_cast<unsigned char>(j[i]))) ++i;
    if (i >= j.size() || j[i] != ':') {
      ++pos;
      continue;
    }
    ++i;
    while (i < j.size() && std::isspace(static_cast<unsigned char>(j[i]))) ++i;
    if (i >= j.size()) return false;
    if (!std::isdigit(static_cast<unsigned char>(j[i]))) {
      ++pos;
      continue;
    }
    long long v = 0;
    while (i < j.size() && std::isdigit(static_cast<unsigned char>(j[i]))) {
      v = v * 10 + (j[i] - '0');
      if (v > INT_MAX) v = INT_MAX;
      ++i;
    }
    out = static_cast<int>(v);
    return true;
  }
  return false;
}

bool json_try_extract_llm_usage_tokens(const std::string& body, int& prompt_tokens, int& completion_tokens,
                                       int& total_tokens) {
  prompt_tokens = completion_tokens = total_tokens = -1;
  int pt = -1, ct = -1, tt = -1, inp = -1, out_tok = -1;
  if (json_first_nonnegative_int_after_key(body, "prompt_tokens", pt)) prompt_tokens = pt;
  if (json_first_nonnegative_int_after_key(body, "completion_tokens", ct)) completion_tokens = ct;
  if (json_first_nonnegative_int_after_key(body, "total_tokens", tt)) total_tokens = tt;
  if (json_first_nonnegative_int_after_key(body, "input_tokens", inp)) {
    if (prompt_tokens < 0) prompt_tokens = inp;
  }
  if (json_first_nonnegative_int_after_key(body, "output_tokens", out_tok)) {
    if (completion_tokens < 0) completion_tokens = out_tok;
  }
  if (total_tokens < 0 && prompt_tokens >= 0 && completion_tokens >= 0)
    total_tokens = prompt_tokens + completion_tokens;
  return prompt_tokens >= 0 || completion_tokens >= 0 || total_tokens >= 0;
}

}  // namespace cct::util
