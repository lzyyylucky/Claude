#include "http_util.hpp"

#ifndef _WIN32

namespace cct::web::http {

bool read_http_request(SOCKET, HttpReq&) { return false; }
void send_all(SOCKET, const std::string&) {}
void respond_text(SOCKET, int, const std::string&, const std::string&, const std::string&) {}
void respond_json(SOCKET, int, const std::string&, const std::string&) {}
std::string cookie_value(const HttpReq&, const std::string&) { return {}; }
std::string http_reason(int) { return "OK"; }

}

#else

#include <cctype>
#include <cstdio>
#include <iostream>
#include <sstream>

namespace cct::web::http {

namespace {

std::string to_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool parse_http_headers(const std::string& head, HttpReq& req) {
  std::istringstream in(head);
  std::string line;
  if (!std::getline(in, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream fl(line);
  if (!(fl >> req.method)) return false;
  std::string fullpath;
  if (!(fl >> fullpath)) return false;
  size_t q = fullpath.find('?');
  if (q != std::string::npos) {
    req.path = fullpath.substr(0, q);
    req.query = fullpath.substr(q + 1);
  } else {
    req.path = fullpath;
  }
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    size_t c = line.find(':');
    if (c == std::string::npos) continue;
    std::string k = line.substr(0, c);
    std::string v = line.substr(c + 1);
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(v.begin());
    req.headers[to_lower(k)] = v;
  }
  return true;
}

}  // namespace

std::string http_reason(int code) {
  switch (code) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 413:
      return "Payload Too Large";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    default:
      return "Error";
  }
}

bool read_http_request(SOCKET s, HttpReq& req) {
  std::string buf;
  char chunk[4096];
  for (;;) {
    int n = recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
    if (n <= 0) return false;
    buf.append(chunk, chunk + n);
    size_t sep = buf.find("\r\n\r\n");
    if (sep != std::string::npos) {
      std::string head = buf.substr(0, sep);
      size_t body_start = sep + 4;
      if (!parse_http_headers(head, req)) return false;
      int cl = 0;
      auto hit = req.headers.find("content-length");
      if (hit != req.headers.end()) {
        try {
          cl = std::stoi(hit->second);
        } catch (...) {
          cl = 0;
        }
      }
      req.body = buf.substr(body_start);
      while (static_cast<int>(req.body.size()) < cl) {
        int m = recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (m <= 0) return false;
        req.body.append(chunk, chunk + m);
      }
      if (static_cast<int>(req.body.size()) > cl) req.body.resize(static_cast<size_t>(cl));
      return true;
    }
    if (buf.size() > 65536) return false;
  }
}

void send_all(SOCKET s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    int n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

void respond_text(SOCKET s, int code, const std::string& content_type, const std::string& body,
                  const std::string& extra_headers) {
  std::ostringstream o;
  o << "HTTP/1.1 " << code << " " << http_reason(code) << "\r\n";
  o << "Content-Type: " << content_type << "\r\n";
  o << "Content-Length: " << body.size() << "\r\n";
  o << "Connection: close\r\n";
  if (!extra_headers.empty()) o << extra_headers;
  o << "\r\n";
  o << body;
  send_all(s, o.str());
}

void respond_json(SOCKET s, int code, const std::string& json, const std::string& extra_headers) {
  respond_text(s, code, "application/json; charset=utf-8", json, extra_headers);
}

void begin_chunked_response(SOCKET s, const std::string& content_type, const std::string& extra_headers) {
  std::ostringstream o;
  o << "HTTP/1.1 200 OK\r\n";
  o << "Content-Type: " << content_type << "\r\n";
  o << "Cache-Control: no-cache\r\n";
  o << "Connection: close\r\n";
  o << "Transfer-Encoding: chunked\r\n";
  o << "X-Content-Type-Options: nosniff\r\n";
  if (!extra_headers.empty()) o << extra_headers;
  o << "\r\n";
  send_all(s, o.str());
}

void send_http_chunk(SOCKET s, const std::string& data) {
  if (data.empty()) return;
  char hx[24];
  const unsigned long long n = static_cast<unsigned long long>(data.size());
#if defined(_MSC_VER)
  sprintf_s(hx, sizeof(hx), "%llx", n);
#else
  std::snprintf(hx, sizeof(hx), "%llx", n);
#endif
  send_all(s, std::string(hx) + "\r\n");
  send_all(s, data);
  send_all(s, "\r\n");
}

void end_chunked_response(SOCKET s) { send_all(s, "0\r\n\r\n"); }

std::string cookie_value(const HttpReq& req, const std::string& name) {
  auto it = req.headers.find("cookie");
  if (it == req.headers.end()) return {};
  const std::string& ck = it->second;
  std::string pat = name + "=";
  size_t pos = 0;
  while (pos < ck.size()) {
    size_t semi = ck.find(';', pos);
    std::string part = semi == std::string::npos ? ck.substr(pos) : ck.substr(pos, semi - pos);
    while (!part.empty() && part[0] == ' ') part.erase(part.begin());
    if (part.size() >= pat.size() && part.compare(0, pat.size(), pat) == 0) return part.substr(pat.size());
    if (semi == std::string::npos) break;
    pos = semi + 1;
  }
  return {};
}

}

#endif
