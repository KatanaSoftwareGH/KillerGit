#include "util.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>

std::wstring widen(const std::string &s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string narrow(const std::wstring &w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
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
  std::time_t tt = (std::time_t)t; std::tm tm{}; localtime_s(&tm, &tt);
  char buf[64]; strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm); return buf;
}
std::string base64(const std::string &b) {
  DWORD n = 0;
  CryptBinaryToStringA((const BYTE *)b.data(), (DWORD)b.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
  std::string s(n, 0);
  CryptBinaryToStringA((const BYTE *)b.data(), (DWORD)b.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &n);
  s.resize(n); return s;
}
std::string unbase64(const std::string &t) {
  DWORD n = 0;
  if (!CryptStringToBinaryA(t.c_str(), (DWORD)t.size(), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return {};
  std::string s(n, 0);
  CryptStringToBinaryA(t.c_str(), (DWORD)t.size(), CRYPT_STRING_BASE64, (BYTE *)s.data(), &n, nullptr, nullptr);
  s.resize(n); return s;
}
std::string urlEncode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF"; std::string r;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

// ---------- processes ----------
// Quotes one argument per the MSVC CommandLineToArgvW rules.
static std::wstring quoteArg(const std::wstring &a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring r = L"\"";
  for (size_t i = 0;; i++) {
    size_t bs = 0;
    while (i < a.size() && a[i] == L'\\') { i++; bs++; }
    if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
    if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; }
    else { r.append(bs, L'\\'); r += a[i]; }
  }
  return r + L"\"";
}

static std::wstring envBlock(const std::map<std::string, std::string> &extra) {
  std::map<std::wstring, std::wstring, std::less<>> vars;  // case-insensitive-ish: Windows env keys
  auto key = [](std::wstring k) { for (auto &c : k) c = towupper(c); return k; };
  std::map<std::wstring, std::pair<std::wstring, std::wstring>> m;
  wchar_t *e = GetEnvironmentStringsW();
  for (wchar_t *p = e; *p; p += wcslen(p) + 1) {
    std::wstring kv = p; size_t eq = kv.find(L'=', 1);
    if (eq == std::wstring::npos) continue;
    m[key(kv.substr(0, eq))] = { kv.substr(0, eq), kv.substr(eq + 1) };
  }
  FreeEnvironmentStringsW(e);
  for (auto &[k, v] : extra) m[key(widen(k))] = { widen(k), widen(v) };
  std::wstring block;
  for (auto &[_, kv] : m) { block += kv.first + L"=" + kv.second; block.push_back(0); }
  block.push_back(0);
  return block;
}

static std::wstring resolveExe(const std::string &exe) {
  std::wstring w = widen(exe);
  if (w.find_first_of(L"\\/") != std::wstring::npos) return w;
  wchar_t buf[MAX_PATH];
  for (const wchar_t *ext : { L".exe", L".cmd", L".bat", L"" })
    if (SearchPathW(nullptr, w.c_str(), ext, MAX_PATH, buf, nullptr)) return buf;
  return w;
}

ProcResult runProcess(const std::string &exe, const Strings &args, const ProcOptions &opt) {
  SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
  HANDLE outR, outW, errR, errW, inR, inW;
  CreatePipe(&outR, &outW, &sa, 0); SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  CreatePipe(&errR, &errW, &sa, 0); SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
  CreatePipe(&inR, &inW, &sa, 0);   SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);

  std::wstring app = resolveExe(exe);
  std::wstring cmd = quoteArg(app);
  for (auto &a : args) cmd += L" " + quoteArg(widen(a));
  std::wstring env = envBlock(opt.env);
  std::wstring cwd = widen(opt.cwd);

  STARTUPINFOW si{ sizeof si };
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = errW;
  PROCESS_INFORMATION pi{};
  bool batch = endsWith(lower(narrow(app)), ".cmd") || endsWith(lower(narrow(app)), ".bat");
  std::wstring full = batch ? L"cmd.exe /d /s /c \"" + cmd + L"\"" : cmd;
  BOOL ok = CreateProcessW(batch ? nullptr : app.c_str(), full.data(), nullptr, nullptr, TRUE,
    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  CloseHandle(outW); CloseHandle(errW); CloseHandle(inR);
  ProcResult r;
  if (!ok) {
    CloseHandle(outR); CloseHandle(errR); CloseHandle(inW);
    throw std::runtime_error("Could not start " + exe + " (error " + std::to_string(GetLastError()) + ")");
  }
  std::thread writer([&] {
    DWORD w; if (!opt.input.empty()) WriteFile(inW, opt.input.data(), (DWORD)opt.input.size(), &w, nullptr);
    CloseHandle(inW);
  });
  std::thread errReader([&] {
    char buf[4096]; DWORD n;
    while (ReadFile(errR, buf, sizeof buf, &n, nullptr) && n) {
      std::string chunk(buf, n); r.err += chunk;
      if (opt.onStderr) opt.onStderr(chunk);
    }
  });
  char buf[65536]; DWORD n;
  while (ReadFile(outR, buf, sizeof buf, &n, nullptr) && n) r.out.append(buf, n);
  writer.join(); errReader.join();
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); r.code = (int)code;
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(outR); CloseHandle(errR);
  return r;
}

void launchDetached(const std::string &cmdline, const std::string &cwd) {
  STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{};
  std::wstring c = widen(cmdline), d = widen(cwd);
  if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, d.empty() ? nullptr : d.c_str(), &si, &pi)) {
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  }
}

// ---------- files ----------
bool fileExists(const std::string &p) { DWORD a = GetFileAttributesW(widen(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool dirExists(const std::string &p) { DWORD a = GetFileAttributesW(widen(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
std::string readFile(const std::string &p) {
  HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot read " + p);
  LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
  std::string s((size_t)sz.QuadPart, 0); DWORD n = 0;
  if (!s.empty()) ReadFile(h, s.data(), (DWORD)s.size(), &n, nullptr);
  CloseHandle(h); s.resize(n); return s;
}
void writeFile(const std::string &p, const std::string &d) {
  std::string tmp = p + ".tmp";
  HANDLE h = CreateFileW(widen(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write " + p);
  DWORD n; WriteFile(h, d.data(), (DWORD)d.size(), &n, nullptr); CloseHandle(h);
  MoveFileExW(widen(tmp).c_str(), widen(p).c_str(), MOVEFILE_REPLACE_EXISTING);
}
void appendFile(const std::string &p, const std::string &d) {
  HANDLE h = CreateFileW(widen(p).c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write " + p);
  DWORD n; WriteFile(h, d.data(), (DWORD)d.size(), &n, nullptr); CloseHandle(h);
}
void makeDirs(const std::string &p) { SHCreateDirectoryExW(nullptr, widen(toBackslash(p)).c_str(), nullptr); }
Strings listDir(const std::string &p) {
  Strings r; WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(widen(p + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return r;
  do { std::string n = narrow(fd.cFileName); if (n != "." && n != "..") r.push_back(n); } while (FindNextFileW(h, &fd));
  FindClose(h); return r;
}
static std::string knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr; std::string r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = narrow(p);
  CoTaskMemFree(p); return r;
}
std::string roamingDir() { return knownFolder(FOLDERID_RoamingAppData); }
std::string appDataDir() { std::string d = roamingDir() + "\\KillerGit"; makeDirs(d); return d; }
std::string homeDir() { return knownFolder(FOLDERID_Profile); }
std::string documentsDir() { return knownFolder(FOLDERID_Documents); }

// ---------- shell ----------
void openPath(const std::string &p) { ShellExecuteW(nullptr, L"open", widen(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void showInExplorer(const std::string &p) {
  std::wstring args = L"/select,\"" + widen(toBackslash(p)) + L"\"";
  ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}
void openUrl(const std::string &u) { if (startsWith(u, "http://") || startsWith(u, "https://")) openPath(u); }
void copyToClipboard(const std::string &s) {
  std::wstring w = widen(s);
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g);
  SetClipboardData(CF_UNICODETEXT, g); CloseClipboard();
}
static std::string pickItem(const std::string &title, bool folder) {
  IFileOpenDialog *dlg = nullptr; std::string r;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return r;
  DWORD opts; dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FORCESHOWHIDDEN));
  if (!title.empty()) dlg->SetTitle(widen(title).c_str());
  if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
    IShellItem *item; if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = narrow(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release(); return r;
}
std::string pickFolder(const std::string &t) { return pickItem(t, true); }
std::string pickFile(const std::string &t) { return pickItem(t, false); }
