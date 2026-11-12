// `KillerGit.exe --mcp`: a Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).
// Lets Claude Code (or any MCP client) inspect repos and fetch/pull/push using KillerGit's stored
// tokens and SSH keys. Credentials are used inside this process and never returned to the client.
#include "git.h"
#include "providers.h"
#include "vault.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <algorithm>

namespace {

#ifdef _WIN32
HANDLE in, out;
bool openStdio() {
  in = GetStdHandle(STD_INPUT_HANDLE); out = GetStdHandle(STD_OUTPUT_HANDLE);
  return in && in != INVALID_HANDLE_VALUE && out && out != INVALID_HANDLE_VALUE;
}
long readIn(char *buf, size_t size) { DWORD n = 0; return ReadFile(in, buf, (DWORD)size, &n, nullptr) ? (long)n : -1; }
void writeOut(const std::string &s) { DWORD n; WriteFile(out, s.data(), (DWORD)s.size(), &n, nullptr); }
#else
bool openStdio() { return true; }
long readIn(char *buf, size_t size) { return (long)read(0, buf, size); }
void writeOut(const std::string &s) {
  size_t off = 0;
  while (off < s.size()) { ssize_t n = write(1, s.data() + off, s.size() - off); if (n <= 0) break; off += n; }
}
#endif

void send(const json &msg) { writeOut(msg.dump() + "\n"); }

std::string arg(const json &a, const char *k, const std::string &def = "") { return a.contains(k) && a[k].is_string() ? a[k].get<std::string>() : def; }

std::string requireRepo(const json &a) {
  std::string r = arg(a, "repo");
  if (r.empty()) throw std::runtime_error("'repo' (absolute path to a git working tree) is required");
  return r;
}
std::string currentBranch(const std::string &repo) {
  std::string b = git::status(repo).branch;
  if (b.empty() || startsWith(b, "HEAD")) throw std::runtime_error("Detached HEAD: pass 'branch' explicitly");
  return b;
}

// git redraws progress lines with \r ("Counting objects:  3%\r…  6%\r…"). Keep only the final state of
// each line so tool results read like a terminal after the command finished.
std::string cleanProgress(const std::string &log) {
  std::string out;
  for (auto &line : split(log, '\n')) {
    size_t cr = line.find_last_of('\r', line.size() > 0 && line.back() == '\r' ? line.size() - 2 : std::string::npos);
    std::string last = trim(cr == std::string::npos ? line : line.substr(cr + 1));
    if (!last.empty()) out += last + "\n";
  }
  return trim(out);
}

json prop(const char *type, const char *desc) { return { { "type", type }, { "description", desc } }; }

struct Tool { const char *name, *desc; json props; Strings required; bool readOnly; std::function<std::string(const json &)> run; };

std::vector<Tool> tools() {
  json repo = prop("string", "Absolute path to the git repository");
  json remote = prop("string", "Remote name (default: origin)");
  json branch = prop("string", "Branch (default: current branch)");
  return {
    { "list_repos", "List the repositories registered in KillerGit, with their remotes.", json::object(), {}, true, [](const json &) {
        json r = json::array();
        for (auto &p : vault::settings().value("repos", Strings{})) {
          json e = { { "path", p } };
          try { json rs = json::array(); for (auto &x : git::remotes(p)) rs.push_back({ { "name", x.name }, { "url", x.fetch } }); e["remotes"] = rs; } catch (...) { e["missing"] = true; }
          r.push_back(e);
        }
        return r.dump(2);
      } },
    { "git_status", "Branch, upstream, ahead/behind counts and changed files.", { { "repo", repo } }, { "repo" }, true, [](const json &a) {
        auto st = git::status(requireRepo(a));
        json f = json::array();
        for (auto &x : st.files) f.push_back({ { "path", x.path }, { "index", std::string(1, x.index) }, { "worktree", std::string(1, x.work) } });
        return json({ { "branch", st.branch }, { "upstream", st.upstream }, { "ahead", st.ahead }, { "behind", st.behind }, { "files", f } }).dump(2);
      } },
    { "git_log", "Recent commits (hash, parents, author, date, refs, subject).", { { "repo", repo }, { "limit", prop("number", "Max commits (default 50, max 1000)") },
        { "all", prop("boolean", "Include all branches (default true)") } }, { "repo" }, true, [](const json &a) {
        int limit = std::clamp(a.value("limit", 50), 1, 1000);
        json r = json::array();
        for (auto &c : git::log(requireRepo(a), 0, limit, a.value("all", true)))
          r.push_back({ { "hash", c.hash }, { "parents", c.parents }, { "author", c.author }, { "time", c.time }, { "refs", c.refs }, { "subject", c.subject } });
        return r.dump(2);
      } },
    { "git_diff", "Working-tree or staged diff, optionally limited to one file.", { { "repo", repo }, { "file", prop("string", "Path relative to the repo") },
        { "staged", prop("boolean", "Show staged changes instead of unstaged") } }, { "repo" }, true, [](const json &a) {
        Strings args = { "diff" }; if (a.value("staged", false)) args.push_back("--cached");
        std::string f = arg(a, "file"); if (!f.empty()) { args.push_back("--"); args.push_back(f); }
        std::string d = git::run(requireRepo(a), args);
        return d.empty() ? std::string("(no changes)") : d;
      } },
    { "git_show", "Show a commit's message, stats and patch.", { { "repo", repo }, { "hash", prop("string", "Commit hash or ref") } }, { "repo", "hash" }, true,
      [](const json &a) { return git::run(requireRepo(a), { "show", "--stat", "--patch", arg(a, "hash") }); } },
    { "git_branches", "Local branches, remote branches and tags with tracking info.", { { "repo", repo } }, { "repo" }, true, [](const json &a) {
        json r = json::array();
        for (auto &b : git::branches(requireRepo(a)))
          r.push_back({ { "name", b.name }, { "type", b.type }, { "current", b.current }, { "upstream", b.upstream }, { "track", b.track }, { "date", b.date } });
        return r.dump(2);
      } },
    { "git_remotes", "Configured remotes and their URLs.", { { "repo", repo } }, { "repo" }, true, [](const json &a) {
        json r = json::array();
        for (auto &x : git::remotes(requireRepo(a))) r.push_back({ { "name", x.name }, { "fetch", x.fetch }, { "push", x.push } });
        return r.dump(2);
      } },
    { "git_fetch", "Fetch (with prune) using KillerGit's stored credentials. Omit 'remote' to fetch every remote.", { { "repo", repo }, { "remote", remote } },
      { "repo" }, false, [](const json &a) {
        std::string r = requireRepo(a), rem = arg(a, "remote"), log;
        Progress p = [&](const std::string &s) { log += s; };
        if (rem.empty()) git::fetchAll(r, p); else git::fetch(r, rem, p);
        return "Fetch complete.\n" + cleanProgress(log);
      } },
    { "git_pull", "Pull a branch using KillerGit's stored credentials (merge by default).", { { "repo", repo }, { "remote", remote }, { "branch", branch },
        { "rebase", prop("boolean", "Rebase instead of merge") } }, { "repo" }, false, [](const json &a) {
        std::string r = requireRepo(a), rem = arg(a, "remote", "origin"), b = arg(a, "branch"), log;
        if (b.empty()) b = currentBranch(r);
        git::pull(r, rem, b, a.value("rebase", false), [&](const std::string &s) { log += s; });
        return "Pulled " + rem + "/" + b + ".\n" + cleanProgress(log);
      } },
    { "git_push", "Push a branch using KillerGit's stored credentials. Never force-pushes.", { { "repo", repo }, { "remote", remote }, { "branch", branch },
        { "setUpstream", prop("boolean", "Set the upstream (-u)") }, { "tags", prop("boolean", "Also push annotated tags (--follow-tags)") } }, { "repo" }, false,
      [](const json &a) {
        std::string r = requireRepo(a), rem = arg(a, "remote", "origin"), b = arg(a, "branch"), log;
        if (b.empty()) b = currentBranch(r);
        git::PushOpts o; o.setUpstream = a.value("setUpstream", false); o.tags = a.value("tags", false);
        git::push(r, rem, b, o, [&](const std::string &s) { log += s; });
        return "Pushed " + b + " to " + rem + ".\n" + cleanProgress(log);
      } },
  };
}

json handle(const json &msg, const std::vector<Tool> &ts) {
  std::string m = msg.value("method", "");
  if (m == "initialize")
    return { { "protocolVersion", msg.contains("params") ? msg["params"].value("protocolVersion", "2024-11-05") : "2024-11-05" },
             { "capabilities", { { "tools", json::object() } } }, { "serverInfo", { { "name", "killergit" }, { "version", "0.2.0" } } } };
  if (m == "ping") return json::object();
  if (m == "tools/list") {
    json list = json::array();
    for (auto &t : ts) {
      json schema = { { "type", "object" }, { "properties", t.props.is_null() ? json::object() : t.props }, { "required", t.required } };
      list.push_back({ { "name", t.name }, { "description", t.desc }, { "inputSchema", schema },
                       { "annotations", { { "readOnlyHint", t.readOnly }, { "destructiveHint", false } } } });
    }
    return { { "tools", list } };
  }
  if (m == "tools/call") {
    std::string name = msg["params"].value("name", "");
    json args = msg["params"].contains("arguments") ? msg["params"]["arguments"] : json::object();
    for (auto &t : ts) if (name == t.name) {
      try { return { { "content", { { { "type", "text" }, { "text", t.run(args) } } } } }; }
      catch (std::exception &e) { return { { "isError", true }, { "content", { { { "type", "text" }, { "text", e.what() } } } } }; }
    }
    throw std::runtime_error("Unknown tool: " + name);
  }
  throw std::invalid_argument("Method not found: " + m);
}

}  // namespace

int runMcp() {
  if (!openStdio()) return 2;
  auto ts = tools();
  std::string buf; char chunk[8192]; long n;
  while ((n = readIn(chunk, sizeof chunk)) > 0) {
    buf.append(chunk, n);
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = trim(buf.substr(0, nl)); buf.erase(0, nl + 1);
      if (line.empty()) continue;
      json msg = json::parse(line, nullptr, false);
      if (msg.is_discarded()) { send({ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", { { "code", -32700 }, { "message", "Parse error" } } } }); continue; }
      bool isRequest = msg.contains("id") && !msg["id"].is_null();
      try {
        json result = handle(msg, ts);
        if (isRequest) send({ { "jsonrpc", "2.0" }, { "id", msg["id"] }, { "result", result } });
      } catch (std::invalid_argument &e) {
        if (isRequest) send({ { "jsonrpc", "2.0" }, { "id", msg["id"] }, { "error", { { "code", -32601 }, { "message", e.what() } } } });
      } catch (std::exception &e) {
        if (isRequest) send({ { "jsonrpc", "2.0" }, { "id", msg["id"] }, { "error", { { "code", -32602 }, { "message", e.what() } } } });
      }
    }
  }
  return 0;
}
