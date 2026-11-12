// Finds repositories known to GitHub Desktop: repositories.json, its IndexedDB (LevelDB tables with
// Snappy-compressed blocks) and its log files. Paths are pulled out of the raw bytes and kept if they exist.
#include "app.h"
#include <windows.h>
#include <regex>
#include <set>

static size_t readVarint(const std::string &b, size_t pos, uint64_t &v) {
  v = 0; int shift = 0;
  while (pos < b.size()) { uint8_t c = b[pos++]; v |= (uint64_t)(c & 0x7f) << shift; if (!(c & 0x80)) break; shift += 7; }
  return pos;
}

static bool snappy(const std::string &in, std::string &out) {
  uint64_t len; size_t pos = readVarint(in, 0, len);
  if (len == 0 || len > 10 * 1024 * 1024) return false;
  out.assign((size_t)len, 0); size_t o = 0;
  auto copy = [&](size_t off, size_t n) { if (!off || off > o) return false; for (size_t i = 0; i < n && o < len; i++, o++) out[o] = out[o - off]; return true; };
  while (pos < in.size() && o < len) {
    uint8_t tag = in[pos++]; int type = tag & 3;
    if (type == 0) {
      size_t lit = (tag >> 2) + 1;
      if (lit > 60) { size_t extra = lit - 60; lit = 0; for (size_t i = 0; i < extra && pos < in.size(); i++) lit |= (size_t)(uint8_t)in[pos++] << (8 * i); lit += 1; }
      if (pos + lit > in.size() || o + lit > len) break;
      memcpy(&out[o], &in[pos], lit); pos += lit; o += lit;
    } else if (type == 1) {
      if (pos >= in.size()) break;
      size_t n = ((tag >> 2) & 7) + 4, off = ((size_t)(tag >> 5) << 8) | (uint8_t)in[pos++];
      if (!copy(off, n)) break;
    } else if (type == 2) {
      if (pos + 2 > in.size()) break;
      size_t n = (tag >> 2) + 1, off = (uint8_t)in[pos] | ((uint8_t)in[pos + 1] << 8); pos += 2;
      if (!copy(off, n)) break;
    } else {
      if (pos + 4 > in.size()) break;
      size_t n = (tag >> 2) + 1, off = (uint8_t)in[pos] | ((uint8_t)in[pos + 1] << 8) | ((uint8_t)in[pos + 2] << 16) | ((size_t)(uint8_t)in[pos + 3] << 24); pos += 4;
      if (!copy(off, n)) break;
    }
  }
  out.resize(o); return true;
}

// LevelDB table: footer (48 bytes, magic at the end) -> index block -> data block handles.
static std::vector<std::pair<size_t, size_t>> tableBlocks(const std::string &b) {
  std::vector<std::pair<size_t, size_t>> r;
  if (b.size() < 48) return r;
  uint64_t magic; memcpy(&magic, &b[b.size() - 8], 8);
  if (magic != 0xdb4775248b80fb57ULL) return r;
  uint64_t x, idxOff, idxSize;
  size_t p = readVarint(b, b.size() - 48, x); p = readVarint(b, p, x); p = readVarint(b, p, idxOff); readVarint(b, p, idxSize);
  if (idxOff + idxSize + 1 > b.size()) return r;
  std::string idx = b.substr((size_t)idxOff, (size_t)idxSize), dec;
  if (b[(size_t)(idxOff + idxSize)] == 1 && snappy(idx, dec)) idx = dec;
  size_t ip = 0;
  while (ip + 4 < idx.size()) {
    uint64_t shared, nonShared, valLen;
    size_t i1 = readVarint(idx, ip, shared), i2 = readVarint(idx, i1, nonShared), i3 = readVarint(idx, i2, valLen);
    if (!valLen || i3 + nonShared + valLen > idx.size()) break;
    uint64_t off, size; size_t v = readVarint(idx, (size_t)(i3 + nonShared), off); readVarint(idx, v, size);
    r.push_back({ (size_t)off, (size_t)size });
    ip = (size_t)(i3 + nonShared + valLen);
  }
  return r;
}

static void extract(const std::string &text, std::set<std::string> &found) {
  static const std::regex winPath("[a-zA-Z]:\\\\[^\\x00-\\x1f\"*?<>|;]+");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), winPath); it != std::sregex_iterator(); ++it) {
    std::string p = trim(it->str());
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    if (endsWith(p, "\\.git")) p = dirName(p);
    // Paths often run into trailing garbage; trim back to the longest prefix that is a repo.
    for (int tries = 0; tries < 6 && !p.empty(); tries++) {
      if (dirExists(p + "\\.git") || fileExists(p + "\\.git")) { found.insert(p); break; }
      size_t cut = p.find_last_of("\\ ");
      if (cut == std::string::npos || cut < 3) break;
      p = p.substr(0, cut);
    }
  }
}

static std::string utf16ToUtf8(const std::string &b) {
  std::wstring w(b.size() / 2, 0); memcpy(w.data(), b.data(), w.size() * 2);
  for (auto &c : w) if (c == 0) c = L' ';
  return narrow(w);
}

Strings githubDesktopRepos() {
  std::set<std::string> found;
  std::string base = roamingDir() + "\\GitHub Desktop";
  if (!dirExists(base)) return {};
  try {
    json j = json::parse(readFile(base + "\\repositories.json"));
    for (auto &r : j) { std::string p = r.is_string() ? r.get<std::string>() : r.value("path", ""); if (dirExists(p)) found.insert(p); }
  } catch (...) {}
  std::string idb = base + "\\IndexedDB\\file__0.indexeddb.leveldb";
  for (auto &f : listDir(idb)) {
    if (!endsWith(f, ".ldb") && !endsWith(f, ".log")) continue;
    std::string buf; try { buf = readFile(idb + "\\" + f); } catch (...) { continue; }
    auto blocks = tableBlocks(buf);
    if (blocks.empty()) { extract(buf, found); extract(utf16ToUtf8(buf), found); continue; }
    for (auto &[off, size] : blocks) {
      if (off + size >= buf.size()) continue;
      std::string raw = buf.substr(off, size), data;
      if (buf[off + size] == 1) { if (!snappy(raw, data)) continue; } else data = raw;
      extract(data, found); extract(utf16ToUtf8(data), found);
    }
  }
  for (auto &f : listDir(base + "\\logs")) {
    if (!endsWith(f, ".log")) continue;
    try { extract(readFile(base + "\\logs\\" + f), found); } catch (...) {}
  }
  return Strings(found.begin(), found.end());
}
