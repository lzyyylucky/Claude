#pragma once

#include <string>
#include <vector>

namespace cct::web::crypto {

bool random_bytes(std::vector<unsigned char>& out, size_t n);
std::string bytes_to_hex(const unsigned char* p, size_t n);
bool hex_to_bytes(const std::string& hex, std::vector<unsigned char>& out);
bool pbkdf2_sha256(const std::string& password, const std::vector<unsigned char>& salt,
                   std::vector<unsigned char>& out32);
bool timing_equal_hex(const std::string& a, const std::string& b);

}
