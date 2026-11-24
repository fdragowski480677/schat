// schat wire protocol: one message per line, fields separated by TAB.
// TAB, LF, CR and backslash inside a field are escaped (\t \n \r \\).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace schat {

constexpr const char* kVersion = "2.0.0";
constexpr int kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 6969;      // TCP chat port
constexpr uint16_t kDiscoveryPort = 6970;    // UDP discovery port
constexpr size_t kMaxLine = 16u * 1024 * 1024;
constexpr size_t kMaxFile = 8u * 1024 * 1024;  // max file size for /send
constexpr size_t kMaxText = 2000;              // max chars per chat message
constexpr const char* kDefaultRoom = "#lobby";

std::string escapeField(const std::string& s);
std::string unescapeField(const std::string& s);
std::string encode(const std::vector<std::string>& fields);  // adds '\n'
std::vector<std::string> decode(const std::string& line);    // line w/o '\n'

bool validNick(const std::string& n);
bool validRoom(const std::string& r);
std::string normalizeRoom(std::string r);  // "Foo" -> "#foo"
std::string lower(std::string s);

std::string base64Encode(const std::string& data);
bool base64Decode(const std::string& in, std::string& out);

// Accumulates bytes from a stream socket and splits them into lines.
class LineBuffer {
 public:
  void append(const char* data, size_t len) { buf_.append(data, len); }
  bool next(std::string& line);
  bool overflow() const { return buf_.size() > kMaxLine; }

 private:
  std::string buf_;
  size_t scanned_ = 0;
};

int64_t nowMs();
int64_t nowSec();
std::string clockStr(int64_t epochSec);     // "14:03"
std::string dateTimeStr(int64_t epochSec);  // "2026-11-25 14:03:07"
std::string humanSize(uint64_t bytes);

}  // namespace schat
