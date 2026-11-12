// Platform-independent helpers. Process, file and shell functions live in util_win.cpp / util_posix.cpp.
#include "util.h"
#include <algorithm>
#include <cstring>
#include <ctime>

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
Strings split(const std::string &s, char sep, bool keepEmpty) {
  Strings out; size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == sep) {
      if (keepEmpty || i > start) out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}
bool startsWith(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
bool icontains(const std::string &hay, const std::string &needle) { return needle.empty() || lower(hay).find(lower(needle)) != std::string::npos; }
std::string join(const Strings &v, const std::string &sep) {
  std::string r; for (size_t i = 0; i < v.size(); i++) { if (i) r += sep; r += v[i]; } return r;
}
std::string baseName(const std::string &p) {
  std::string s = p; while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  size_t i = s.find_last_of("\\/"); return i == std::string::npos ? s : s.substr(i + 1);
}
std::string dirName(const std::string &p) { size_t i = p.find_last_of("\\/"); return i == std::string::npos ? "" : p.substr(0, i); }
std::string toBackslash(std::string p) { std::replace(p.begin(), p.end(), '/', '\\'); return p; }

std::string ago(long long t) {
  if (!t) return "";
  long long d = (long long)std::time(nullptr) - t;
  if (d < 60) return "just now";
  if (d < 3600) return std::to_string(d / 60) + "m ago";
  if (d < 86400) return std::to_string(d / 3600) + "h ago";
  if (d < 86400 * 30) return std::to_string(d / 86400) + "d ago";
  if (d < 86400 * 365) return std::to_string(d / (86400 * 30)) + "mo ago";
  return std::to_string(d / (86400 * 365)) + "y ago";
}
std::string formatTime(long long t) {
  std::time_t tt = (std::time_t)t; std::tm tm{}; localTime(tt, tm);
  char buf[64]; strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm); return buf;
}
static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(const std::string &b) {
  std::string r; r.reserve((b.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < b.size(); i += 3) {
    unsigned v = ((unsigned char)b[i] << 16) | ((unsigned char)b[i + 1] << 8) | (unsigned char)b[i + 2];
    r += B64[v >> 18]; r += B64[(v >> 12) & 63]; r += B64[(v >> 6) & 63]; r += B64[v & 63];
  }
  if (i < b.size()) {
    unsigned v = (unsigned char)b[i] << 16; if (i + 1 < b.size()) v |= (unsigned char)b[i + 1] << 8;
    r += B64[v >> 18]; r += B64[(v >> 12) & 63];
    r += i + 1 < b.size() ? B64[(v >> 6) & 63] : '='; r += '=';
  }
  return r;
}
std::string unbase64(const std::string &t) {
  std::string r; unsigned v = 0; int bits = 0;
  for (char c : t) {
    const char *p = c ? strchr(B64, c) : nullptr;
    if (c == '-') p = B64 + 62; else if (c == '_') p = B64 + 63;  // base64url too
    if (!p) continue;
    v = (v << 6) | (unsigned)(p - B64); bits += 6;
    if (bits >= 8) { bits -= 8; r += (char)((v >> bits) & 255); }
  }
  return r;
}
std::string canonicalHost(const std::string &host) {
  std::string h = lower(trim(host));
  for (const char *p : { "ssh.", "altssh." })
    if (startsWith(h, p) && h.find('.', strlen(p)) != std::string::npos) return h.substr(strlen(p));
  return h;
}
std::string urlEncode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF"; std::string r;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

