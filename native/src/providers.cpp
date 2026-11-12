#include "providers.h"
#include "vault.h"
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif
#include <cstdio>
#include <ctime>
#include <regex>

namespace providers {

bool parseRemote(const std::string &url, Parsed &out) {
  static const std::regex a("^[a-z+]+://(?:[^@/]+@)?([^/:]+)(?::\\d+)?/(.+?)(?:\\.git)?/?$", std::regex::icase);
  static const std::regex b("^(?:[^@]+@)?([^:]+):(.+?)(?:\\.git)?$");
  std::smatch m;
  if (!std::regex_match(url, m, a) && !std::regex_match(url, m, b)) return false;
  std::string full = m[2]; while (!full.empty() && full[0] == '/') full.erase(0, 1);
  Strings parts = split(full, '/');
  if (parts.empty()) return false;
  out.host = lower(m[1]); out.fullPath = full; out.name = parts.back();
  parts.pop_back(); out.ns = join(parts, "/");
  return true;
}

// Parses ISO-8601 "2024-05-01T12:34:56Z" / "+02:00" / ".123Z" into unix seconds.
static long long isoTime(const json &v) {
  if (!v.is_string()) return 0;
  std::string s = v.get<std::string>();
  int Y, M, D, h, m, sec; if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  std::tm tm{}; tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec;
  long long t = timegm_portable(&tm);
  size_t z = s.find_first_of("+-", 19);
  if (z != std::string::npos) { int oh = 0, om = 0; sscanf(s.c_str() + z + 1, "%d:%d", &oh, &om); t -= (s[z] == '+' ? 1 : -1) * (oh * 3600 + om * 60); }
  return t;
}

// Sends one API request; returns the HTTP status and fills `text` with the body. Throws on transport errors.
static long httpSend(const std::string &full, const std::string &method, const Strings &headerLines, const std::string &payload, std::string &text) {
#ifdef _WIN32
  URL_COMPONENTS uc{ sizeof uc };
  wchar_t host[256], urlPath[4096];
  uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = urlPath; uc.dwUrlPathLength = 4096;
  std::wstring wurl = widen(full);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) throw std::runtime_error("Bad API URL: " + full);
  std::wstring pathAndQuery = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength);
  HINTERNET s = WinHttpOpen(L"KillerGit", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  HINTERNET c = WinHttpConnect(s, std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(), uc.nPort, 0);
  HINTERNET r = WinHttpOpenRequest(c, widen(method).c_str(), pathAndQuery.c_str(), nullptr, nullptr, nullptr,
                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); throw std::runtime_error("Could not connect to " + full); }
  std::wstring headers;
  for (auto &h : headerLines) headers += widen(h) + L"\r\n";
  BOOL ok = WinHttpSendRequest(r, headers.c_str(), (DWORD)-1L, payload.empty() ? nullptr : (void *)payload.data(), (DWORD)payload.size(), (DWORD)payload.size(), 0)
            && WinHttpReceiveResponse(r, nullptr);
  DWORD statusCode = 0, sz = sizeof statusCode;
  if (ok) {
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &sz, nullptr);
    DWORD avail;
    while (WinHttpQueryDataAvailable(r, &avail) && avail) {
      std::string buf(avail, 0); DWORD n = 0;
      WinHttpReadData(r, buf.data(), avail, &n); text.append(buf.data(), n);
    }
  }
  DWORD err = GetLastError();
  WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
  if (!ok) throw std::runtime_error("Network error " + std::to_string(err) + " talking to " + narrow(host));
  return (long)statusCode;
#else
  static bool inited = (curl_global_init(CURL_GLOBAL_DEFAULT), true); (void)inited;
  CURL *c = curl_easy_init();
  if (!c) throw std::runtime_error("Could not initialise libcurl");
  curl_slist *hl = nullptr;
  for (auto &h : headerLines) hl = curl_slist_append(hl, h.c_str());
  curl_easy_setopt(c, CURLOPT_URL, full.c_str());
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  if (!payload.empty()) { curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.data()); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)payload.size()); }
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char *p, size_t sz, size_t n, void *ud) -> size_t { ((std::string *)ud)->append(p, sz * n); return sz * n; });
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &text);
  CURLcode rc = curl_easy_perform(c);
  long status = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(hl); curl_easy_cleanup(c);
  if (rc != CURLE_OK) throw std::runtime_error(std::string("Network error: ") + curl_easy_strerror(rc) + " (" + full + ")");
  return status;
#endif
}

static json req(const json &acct, const std::string &method, const std::string &path, const json &body = nullptr) {
  std::string full = acct.value("api", "") + path;
  Strings headers = { "Accept: application/json", "User-Agent: KillerGit" };
  std::string token = acct.value("token", "");
  if (!token.empty()) headers.push_back(acct.value("kind", "") == "github" ? "Authorization: Bearer " + token : "PRIVATE-TOKEN: " + token);
  std::string payload;
  if (!body.is_null()) { payload = body.dump(); headers.push_back("Content-Type: application/json"); }
  std::string text;
  long statusCode = httpSend(full, method, headers, payload, text);
  json j = text.empty() ? json() : json::parse(text, nullptr, false);
  if (statusCode < 200 || statusCode >= 300) {
    std::string msg;
    if (j.is_object()) msg = j.contains("message") ? j["message"].dump() : j.contains("error") ? j["error"].dump() : j.dump();
    throw std::runtime_error(std::to_string(statusCode) + ": " + (msg.empty() ? text : msg));
  }
  return j;
}

static json acctById(const std::string &id) {
  json a = vault::account(id);
  if (!a.is_object()) throw std::runtime_error("Account not found");
  return a;
}
static bool isGh(const json &a) { return a.value("kind", "") == "github"; }
static std::string str(const json &o, const char *k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : ""; }

Me me(const std::string &id) {
  json a = acctById(id), u = req(a, "GET", "/user");
  return { str(u, isGh(a) ? "login" : "username"), str(u, "name") };
}

std::vector<HostRepo> repos(const std::string &id) {
  json a = acctById(id); std::vector<HostRepo> out;
  if (isGh(a)) {
    for (auto &r : req(a, "GET", "/user/repos?per_page=100&sort=updated"))
      out.push_back({ str(r, "name"), str(r, "full_name"), str(r["owner"], "login"), str(r, "ssh_url"), str(r, "clone_url"), str(r, "description"),
                      r.value("private", false), isoTime(r.contains("pushed_at") ? r["pushed_at"] : r["updated_at"]) });
  } else {
    for (auto &r : req(a, "GET", "/projects?membership=true&per_page=100&order_by=last_activity_at"))
      out.push_back({ str(r, "path"), str(r, "path_with_namespace"), str(r["namespace"], "full_path"), str(r, "ssh_url_to_repo"), str(r, "http_url_to_repo"),
                      str(r, "description"), str(r, "visibility") != "public", isoTime(r["last_activity_at"]) });
  }
  return out;
}

std::vector<Namespace> namespaces(const std::string &id) {
  json a = acctById(id); std::vector<Namespace> out;
  if (isGh(a)) {
    out.push_back({ "", me(id).username, "user" });
    try { for (auto &o : req(a, "GET", "/user/orgs")) out.push_back({ str(o, "login"), str(o, "login"), "org" }); } catch (...) {}
  } else {
    for (auto &n : req(a, "GET", "/namespaces?per_page=100")) out.push_back({ std::to_string(n.value("id", 0LL)), str(n, "full_path"), str(n, "kind") });
  }
  return out;
}

Created createRepo(const std::string &id, const std::string &name, const std::string &desc, bool priv, const std::string &ns) {
  json a = acctById(id);
  if (isGh(a)) {
    json r = req(a, "POST", ns.empty() ? "/user/repos" : "/orgs/" + urlEncode(ns) + "/repos", { { "name", name }, { "description", desc }, { "private", priv } });
    return { str(r, "ssh_url"), str(r, "clone_url"), str(r, "html_url") };
  }
  json b = { { "name", name }, { "path", name }, { "description", desc }, { "visibility", priv ? "private" : "public" } };
  if (!ns.empty()) b["namespace_id"] = atoll(ns.c_str());
  json r = req(a, "POST", "/projects", b);
  return { str(r, "ssh_url_to_repo"), str(r, "http_url_to_repo"), str(r, "web_url") };
}

void addSshKey(const std::string &id, const std::string &title, const std::string &key) {
  req(acctById(id), "POST", "/user/keys", { { "title", title }, { "key", key } });
}

static std::pair<json, std::string> forRemote(const std::string &url) {
  Parsed p;
  if (!parseRemote(url, p)) throw std::runtime_error("Unrecognised remote URL");
  json a = vault::accountForHost(p.host);
  if (!a.is_object()) throw std::runtime_error("No account configured for " + p.host + ". Add one in Accounts.");
  return { a, p.fullPath };
}

std::vector<Issue> issues(const std::string &url, const std::string &state) {
  auto [a, p] = forRemote(url); std::vector<Issue> out;
  if (isGh(a)) {
    for (auto &i : req(a, "GET", "/repos/" + p + "/issues?state=" + state + "&per_page=100")) {
      if (i.contains("pull_request")) continue;
      Issue x{ i.value("number", 0LL), str(i, "title"), str(i, "state"), str(i["user"], "login"), str(i, "body"), str(i, "html_url"), isoTime(i["created_at"]), i.value("comments", 0) };
      for (auto &l : i["labels"]) x.labels.push_back(str(l, "name"));
      out.push_back(x);
    }
  } else {
    for (auto &i : req(a, "GET", "/projects/" + urlEncode(p) + "/issues?state=" + (state == "open" ? "opened" : state) + "&per_page=100")) {
      Issue x{ i.value("iid", 0LL), str(i, "title"), str(i, "state") == "opened" ? "open" : "closed", str(i["author"], "username"), str(i, "description"),
               str(i, "web_url"), isoTime(i["created_at"]), i.value("user_notes_count", 0) };
      for (auto &l : i["labels"]) if (l.is_string()) x.labels.push_back(l);
      out.push_back(x);
    }
  }
  return out;
}

std::vector<Comment> comments(const std::string &url, long long id) {
  auto [a, p] = forRemote(url); std::vector<Comment> out;
  if (isGh(a)) {
    for (auto &c : req(a, "GET", "/repos/" + p + "/issues/" + std::to_string(id) + "/comments?per_page=100"))
      out.push_back({ str(c["user"], "login"), str(c, "body"), isoTime(c["created_at"]) });
  } else {
    for (auto &c : req(a, "GET", "/projects/" + urlEncode(p) + "/issues/" + std::to_string(id) + "/notes?per_page=100&sort=asc"))
      if (!c.value("system", false)) out.push_back({ str(c["author"], "username"), str(c, "body"), isoTime(c["created_at"]) });
  }
  return out;
}

void createIssue(const std::string &url, const std::string &title, const std::string &body) {
  auto [a, p] = forRemote(url);
  if (isGh(a)) req(a, "POST", "/repos/" + p + "/issues", { { "title", title }, { "body", body } });
  else req(a, "POST", "/projects/" + urlEncode(p) + "/issues", { { "title", title }, { "description", body } });
}
void comment(const std::string &url, long long id, const std::string &body) {
  auto [a, p] = forRemote(url);
  if (isGh(a)) req(a, "POST", "/repos/" + p + "/issues/" + std::to_string(id) + "/comments", { { "body", body } });
  else req(a, "POST", "/projects/" + urlEncode(p) + "/issues/" + std::to_string(id) + "/notes", { { "body", body } });
}
void setIssueState(const std::string &url, long long id, bool open) {
  auto [a, p] = forRemote(url);
  if (isGh(a)) req(a, "PATCH", "/repos/" + p + "/issues/" + std::to_string(id), { { "state", open ? "open" : "closed" } });
  else req(a, "PUT", "/projects/" + urlEncode(p) + "/issues/" + std::to_string(id), { { "state_event", open ? "reopen" : "close" } });
}

}  // namespace providers
