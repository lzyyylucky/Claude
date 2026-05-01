#include "crypto.hpp"

#ifndef _WIN32

namespace cct::web::crypto {

bool random_bytes(std::vector<unsigned char>&, size_t) { return false; }
std::string bytes_to_hex(const unsigned char*, size_t) { return {}; }
bool hex_to_bytes(const std::string&, std::vector<unsigned char>&) { return false; }
bool pbkdf2_sha256(const std::string&, const std::vector<unsigned char>&, std::vector<unsigned char>&) {
  return false;
}
bool timing_equal_hex(const std::string& a, const std::string& b) { return a == b; }

}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace cct::web::crypto {

bool random_bytes(std::vector<unsigned char>& out, size_t n) {
  out.resize(n);
  return BCRYPT_SUCCESS(
      BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string bytes_to_hex(const unsigned char* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    s[2 * i] = d[p[i] >> 4];
    s[2 * i + 1] = d[p[i] & 15];
  }
  return s;
}

bool hex_to_bytes(const std::string& hex, std::vector<unsigned char>& out) {
  if (hex.size() % 2) return false;
  out.resize(hex.size() / 2);
  auto dig = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < out.size(); ++i) {
    int a = dig(hex[2 * i]);
    int b = dig(hex[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = static_cast<unsigned char>((a << 4) | b);
  }
  return true;
}

bool pbkdf2_sha256(const std::string& password, const std::vector<unsigned char>& salt,
                   std::vector<unsigned char>& out32) {
  out32.resize(32);
  BCRYPT_ALG_HANDLE h = nullptr;
  if (!BCRYPT_SUCCESS(
          BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
    return false;
  NTSTATUS st = BCryptDeriveKeyPBKDF2(
      h, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
      const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), 100000, out32.data(), 32, 0);
  BCryptCloseAlgorithmProvider(h, 0);
  return BCRYPT_SUCCESS(st);
}

bool timing_equal_hex(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char x = 0;
  for (size_t i = 0; i < a.size(); ++i) x |= static_cast<unsigned char>(a[i] ^ b[i]);
  return x == 0;
}

}

#endif
