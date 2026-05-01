#pragma once

#ifndef _WIN32
#include <string>
namespace cct::web::http {
struct HttpReq {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
};
}
#else
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <string>
#include <unordered_map>

namespace cct::web::http {

struct HttpReq {
  std::string method;
  std::string path;
  std::string query;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

bool read_http_request(SOCKET s, HttpReq& req);
void send_all(SOCKET s, const std::string& data);
void respond_text(SOCKET s, int code, const std::string& content_type, const std::string& body,
                  const std::string& extra_headers = {});
void respond_json(SOCKET s, int code, const std::string& json, const std::string& extra_headers = {});
void begin_chunked_response(SOCKET s, const std::string& content_type, const std::string& extra_headers = {});
void send_http_chunk(SOCKET s, const std::string& data);
void end_chunked_response(SOCKET s);
std::string cookie_value(const HttpReq& req, const std::string& name);
std::string http_reason(int code);

}

#endif
