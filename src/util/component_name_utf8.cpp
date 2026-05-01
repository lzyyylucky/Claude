#include "component_name_utf8.hpp"

#include <cstdint>

namespace cct::util {

namespace {

bool utf8_decode_unit(const std::string& n, std::size_t& i, char32_t& out_cp, std::size_t& out_len) {
  if (i >= n.size()) return false;
  unsigned char b0 = static_cast<unsigned char>(n[i]);
  if (b0 < 0x80) {
    out_cp = b0;
    out_len = 1;
    return true;
  }
  auto need = [&](std::size_t k) -> bool {
    if (i + k > n.size()) return false;
    for (std::size_t j = 1; j <= k; ++j) {
      unsigned char bx = static_cast<unsigned char>(n[i + j]);
      if ((bx & 0xC0) != 0x80) return false;
    }
    return true;
  };
  char32_t cp = 0;
  std::size_t len = 0;
  if ((b0 & 0xE0) == 0xC0) {
    if (!need(1)) return false;
    unsigned char b1 = static_cast<unsigned char>(n[i + 1]);
    cp = (static_cast<char32_t>(b0 & 0x1F) << 6) | static_cast<char32_t>(b1 & 0x3F);
    if (cp < 0x80) return false;
    len = 2;
  } else if ((b0 & 0xF0) == 0xE0) {
    if (!need(2)) return false;
    unsigned char b1 = static_cast<unsigned char>(n[i + 1]);
    unsigned char b2 = static_cast<unsigned char>(n[i + 2]);
    cp = (static_cast<char32_t>(b0 & 0x0F) << 12) | (static_cast<char32_t>(b1 & 0x3F) << 6) |
         static_cast<char32_t>(b2 & 0x3F);
    if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    len = 3;
  } else if ((b0 & 0xF8) == 0xF0) {
    if (!need(3)) return false;
    unsigned char b1 = static_cast<unsigned char>(n[i + 1]);
    unsigned char b2 = static_cast<unsigned char>(n[i + 2]);
    unsigned char b3 = static_cast<unsigned char>(n[i + 3]);
    cp = (static_cast<char32_t>(b0 & 0x07) << 18) | (static_cast<char32_t>(b1 & 0x3F) << 12) |
         (static_cast<char32_t>(b2 & 0x3F) << 6) | static_cast<char32_t>(b3 & 0x3F);
    if (cp < 0x10000 || cp > 0x10FFFF) return false;
    len = 4;
  } else
    return false;
  if (len == 2 && cp < 0x80) return false;
  if (len == 3 && cp < 0x800) return false;
  if (len == 4 && cp < 0x10000) return false;
  out_cp = cp;
  out_len = len;
  return true;
}

bool codepoint_allowed_filename(char32_t cp) {
  if (cp < 32) return false;
  if (cp == '<' || cp == '>' || cp == ':' || cp == '"' || cp == '/' || cp == '\\' || cp == '|' || cp == '?' ||
      cp == '*')
    return false;
  return true;
}

}  // namespace

bool valid_component_name_utf8(const std::string& n) {
  if (n.empty() || n.size() > 120) return false;
  if (n == "." || n == "..") return false;
  std::size_t i = 0;
  std::size_t char_count = 0;
  constexpr std::size_t kMaxChars = 120;
  while (i < n.size()) {
    if (char_count >= kMaxChars) return false;
    char32_t cp = 0;
    std::size_t len = 0;
    if (!utf8_decode_unit(n, i, cp, len)) return false;
    if (!codepoint_allowed_filename(cp)) return false;
    i += len;
    ++char_count;
  }
  return true;
}

}  // namespace cct::util
