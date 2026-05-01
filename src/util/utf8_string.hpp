#pragma once

#include <cstddef>
#include <string>

namespace cct::util {

/** 按 Unicode 字符数截断并在必要时追加 UTF-8 省略号（…），避免.substr 切断多字节 UTF-8 */
inline std::string utf8_ellipsis_prefix_chars(const std::string& s, std::size_t max_chars) {
  if (s.empty() || max_chars == 0) return "";
  std::size_t i = 0, n = 0;
  while (i < s.size() && n < max_chars) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if ((c & 0x80u) == 0u) {
      len = 1;
    } else if ((c & 0xE0u) == 0xC0u) {
      len = 2;
    } else if ((c & 0xF0u) == 0xE0u) {
      len = 3;
    } else if ((c & 0xF8u) == 0xF0u) {
      len = 4;
    } else {
      ++i;
      continue;
    }
    if (i + len > s.size()) break;
    i += len;
    ++n;
  }
  std::string out = s.substr(0, i);
  if (i < s.size()) {
    static const char ell[] = "\xe2\x80\xa6";
    out.append(ell, sizeof(ell) - 1);
  }
  return out;
}

/** 按字节截断且不在 UTF-8 多字节字符中间切断 */
inline std::string utf8_safe_truncate_bytes(const std::string& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t end = max_bytes;
  while (end > 0 && end <= s.size() && (static_cast<unsigned char>(s[end - 1]) & 0xC0u) == 0x80u) --end;
  return s.substr(0, end);
}

}  // namespace cct::util
