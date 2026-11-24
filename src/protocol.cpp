#include "protocol.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace schat {

std::string escapeField(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '\t': o += "\\t"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      default: o += c;
    }
  }
  return o;
}

std::string unescapeField(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::string encode(const std::vector<std::string>& fields) {
  std::string o;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) o += '\t';
    o += escapeField(fields[i]);
  }
  o += '\n';
  return o;
}

std::vector<std::string> decode(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  std::string l = line;
  if (!l.empty() && l.back() == '\r') l.pop_back();
  while (true) {
    size_t p = l.find('\t', start);
    out.push_back(unescapeField(l.substr(start, p == std::string::npos ? std::string::npos : p - start)));
    if (p == std::string::npos) break;
    start = p + 1;
  }
  return out;
}

bool validNick(const std::string& n) {
  if (n.empty() || n.size() > 20) return false;
  for (unsigned char c : n)
    if (!std::isalnum(c) && c != '_' && c != '-') return false;
  return true;
}

bool validRoom(const std::string& r) {
  if (r.size() < 2 || r.size() > 25 || r[0] != '#') return false;
  for (size_t i = 1; i < r.size(); ++i) {
    unsigned char c = r[i];
    if (!std::islower(c) && !std::isdigit(c) && c != '_' && c != '-') return false;
  }
  return true;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string normalizeRoom(std::string r) {
  r = lower(r);
  if (r.empty() || r[0] != '#') r = "#" + r;
  return r;
}

static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
    out += kB64[v >> 18 & 63]; out += kB64[v >> 12 & 63];
    out += kB64[v >> 6 & 63];  out += kB64[v & 63];
  }
  size_t rest = in.size() - i;
  if (rest) {
    uint32_t v = (uint8_t)in[i] << 16 | (rest == 2 ? (uint8_t)in[i + 1] << 8 : 0);
    out += kB64[v >> 18 & 63]; out += kB64[v >> 12 & 63];
    out += rest == 2 ? kB64[v >> 6 & 63] : '=';
    out += '=';
  }
  return out;
}

bool base64Decode(const std::string& in, std::string& out) {
  if (in.size() % 4) return false;
  out.clear();
  out.reserve(in.size() / 4 * 3);
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  for (size_t i = 0; i < in.size(); i += 4) {
    int a = val(in[i]), b = val(in[i + 1]);
    bool last = i + 4 == in.size();
    int c = (last && in[i + 2] == '=') ? -2 : val(in[i + 2]);
    int d = (last && in[i + 3] == '=') ? -2 : val(in[i + 3]);
    if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2)) return false;
    uint32_t v = a << 18 | b << 12 | (c < 0 ? 0 : c) << 6 | (d < 0 ? 0 : d);
    out += char(v >> 16 & 255);
    if (c >= 0) out += char(v >> 8 & 255);
    if (d >= 0) out += char(v & 255);
  }
  return true;
}

bool LineBuffer::next(std::string& line) {
  size_t p = buf_.find('\n', scanned_);
  if (p == std::string::npos) {
    scanned_ = buf_.size();
    return false;
  }
  line = buf_.substr(0, p);
  buf_.erase(0, p + 1);
  scanned_ = 0;
  return true;
}

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
int64_t nowSec() { return static_cast<int64_t>(std::time(nullptr)); }

static std::string fmtTime(int64_t t, const char* f) {
  std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
  localtime_r(&tt, &tm);
  char b[64];
  std::strftime(b, sizeof b, f, &tm);
  return b;
}
std::string clockStr(int64_t t) { return fmtTime(t, "%H:%M"); }
std::string dateTimeStr(int64_t t) { return fmtTime(t, "%Y-%m-%d %H:%M:%S"); }

std::string humanSize(uint64_t b) {
  char buf[32];
  if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  else if (b < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", b / 1024.0);
  else std::snprintf(buf, sizeof buf, "%.1f MiB", b / 1048576.0);
  return buf;
}

}  // namespace schat
