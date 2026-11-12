#include "git.h"
#include "ssh.h"
#include "vault.h"
#include <algorithm>
#include <regex>
#include <sstream>

namespace git {

static const char SEP = '\x1f', REC = '\x1e';

std::string hostOf(const std::string &url) {
  std::smatch m;
  static const std::regex a("^[a-z+]+://(?:[^@/]+@)?([^/:]+)", std::regex::icase), b("^(?:[^@]+@)?([^:]+):");
  if (std::regex_search(url, m, a)) return canonicalHost(m[1]);
  if (std::regex_search(url, m, b)) return canonicalHost(m[1]);
  return "";
}

static std::string fwd(std::string p) { for (auto &c : p) if (c == '\\') c = '/'; return p; }

// HTTPS gets an Authorization header from the vault; SSH gets Windows' OpenSSH plus the host's key.
// git runs hidden, so ssh must never prompt (unknown host key, passphrase): fail fast instead.
static void authFor(const std::string &url, Strings &args, std::map<std::string, std::string> &env) {
  if (url.empty()) return;
  json acct = vault::accountForHost(hostOf(url));
  bool https = std::regex_search(url, std::regex("^https?:", std::regex::icase));
  if (https && acct.is_object() && !acct.value("token", "").empty()) {
    std::string user = acct.value("username", "");
    if (user.empty()) user = acct.value("kind", "") == "github" ? "x-access-token" : "oauth2";
    args.insert(args.end(), { "-c", "http.extraHeader=Authorization: Basic " + base64(user + ":" + acct.value("token", "")) });
  }
  std::string cmd = "\"" + fwd(ssh::exe("ssh")) + "\" -o BatchMode=yes -o StrictHostKeyChecking=accept-new"
                    " -o ConnectTimeout=20 -o ServerAliveInterval=15 -o ServerAliveCountMax=4";
  std::string key = acct.is_object() ? acct.value("sshKey", "") : "";
  if (!key.empty()) cmd += " -i \"" + fwd(key) + "\" -o IdentitiesOnly=yes";
  else { int n = 0; for (auto &k : ssh::privateKeys()) { if (n++ >= 5) break; cmd += " -i \"" + fwd(k) + "\""; } }
  env["GIT_SSH_COMMAND"] = cmd;
}

std::string run(const std::string &cwd, const Strings &args, const std::string &url, const Progress &progress, bool allowFail, const std::string &input) {
  Strings full; ProcOptions o;
  authFor(url, full, o.env);
  full.insert(full.end(), args.begin(), args.end());
  o.cwd = cwd; o.input = input; o.onStderr = progress;
  o.env["GIT_TERMINAL_PROMPT"] = "0";
  o.env["LC_ALL"] = "C";
  ProcResult r = runProcess("git", full, o);
  if (r.code != 0 && !allowFail) {
    std::string e = trim(r.err);
    throw std::runtime_error(e.empty() ? "git " + (args.empty() ? "" : args[0]) + " exited " + std::to_string(r.code) : e);
  }
  return r.out;
}

std::string remoteUrl(const std::string &cwd, const std::string &remote) {
  return trim(run(cwd, { "remote", "get-url", remote }, "", nullptr, true));
}

Status status(const std::string &cwd, const std::string &untracked) {
  std::string out = run(cwd, { "status", "--porcelain=v1", "-z", "-b", "-u" + untracked });
  Strings parts = split(out, '\0', true);
  Status st;
  static const std::regex head("^(.+?)(?:\\.\\.\\.(\\S+))?(?: \\[(.*)\\])?$");
  for (size_t i = 0; i < parts.size(); i++) {
    const std::string &e = parts[i];
    if (e.empty()) continue;
    if (startsWith(e, "## ")) {
      std::smatch m; std::string h = e.substr(3);
      if (std::regex_match(h, m, head)) {
        st.branch = m[1]; st.upstream = m[2];
        std::string t = m[3]; std::smatch n;
        if (std::regex_search(t, n, std::regex("ahead (\\d+)"))) st.ahead = std::stoi(n[1]);
        if (std::regex_search(t, n, std::regex("behind (\\d+)"))) st.behind = std::stoi(n[1]);
      }
      const std::string pre = "No commits yet on ";
      if (startsWith(st.branch, pre)) st.branch = st.branch.substr(pre.size());
      continue;
    }
    if (e.size() < 4) continue;
    FileStatus f; f.index = e[0]; f.work = e[1]; f.path = e.substr(3);
    if ((f.index == 'R' || f.index == 'C') && i + 1 < parts.size()) f.orig = parts[++i];
    f.staged = f.index != ' ' && f.index != '?';
    f.unstaged = f.work != ' ';
    st.files.push_back(f);
  }
  return st;
}

std::string diff(const std::string &cwd, const std::string &file, bool staged, bool untracked) {
  if (untracked) return run(cwd, { "diff", "--no-index", "--", "/dev/null", file }, "", nullptr, true);
  Strings a = { "diff" }; if (staged) a.push_back("--cached");
  a.push_back("--"); a.push_back(file);
  return run(cwd, a);
}

std::vector<Commit> log(const std::string &cwd, int skip, int limit, bool all) {
  std::string fmt = std::string("%H") + SEP + "%P" + SEP + "%an" + SEP + "%ae" + SEP + "%at" + SEP + "%D" + SEP + "%s" + REC;
  Strings a = { "log", all ? "--all" : "HEAD", "--topo-order", "--skip=" + std::to_string(skip), "-n" + std::to_string(limit), "--format=" + fmt };
  std::string out = run(cwd, a, "", nullptr, true);
  std::vector<Commit> r;
  for (auto rec : split(out, REC)) {
    while (!rec.empty() && (rec[0] == '\n' || rec[0] == '\r')) rec.erase(0, 1);
    if (rec.empty()) continue;
    Strings p = split(rec, SEP, true);
    if (p.size() < 7) continue;
    Commit c; c.hash = p[0]; c.parents = split(p[1], ' '); c.author = p[2]; c.email = p[3];
    c.time = atoll(p[4].c_str());
    if (!p[5].empty()) { size_t s = 0; std::string refs = p[5];
      while (true) { size_t i = refs.find(", ", s); c.refs.push_back(refs.substr(s, i == std::string::npos ? std::string::npos : i - s)); if (i == std::string::npos) break; s = i + 2; } }
    c.subject = p[6];
    r.push_back(c);
  }
  return r;
}

CommitDetail show(const std::string &cwd, const std::string &hash) {
  std::string meta = run(cwd, { "show", "-s", std::string("--format=%H") + SEP + "%an <%ae>" + SEP + "%at" + SEP + "%B", hash });
  Strings p = split(meta, SEP, true);
  CommitDetail d;
  if (p.size() >= 4) { d.hash = p[0]; d.author = p[1]; d.time = atoll(p[2].c_str()); d.body = p[3]; }
  for (auto &l : split(trim(run(cwd, { "show", "--format=", "--name-status", "-M", hash })), '\n')) {
    Strings q = split(trim(l), '\t');
    if (q.size() >= 2) d.files.push_back({ q[0][0], q.back() });
  }
  return d;
}
std::string showFile(const std::string &cwd, const std::string &hash, const std::string &file) {
  return run(cwd, { "show", "--format=", "-M", hash, "--", file });
}

std::vector<Branch> branches(const std::string &cwd) {
  std::string fmt = std::string("%(refname)") + SEP + "%(refname:short)" + SEP + "%(upstream:short)" + SEP + "%(committerdate:unix)" + SEP + "%(HEAD)" + SEP + "%(upstream:track)";
  std::vector<Branch> r;
  for (auto &l : split(run(cwd, { "for-each-ref", "--format=" + fmt, "refs/heads", "refs/remotes", "refs/tags" }), '\n')) {
    Strings p = split(trim(l), SEP, true);
    if (p.size() < 6) continue;
    Branch b; b.ref = p[0]; b.name = p[1]; b.upstream = p[2]; b.date = atoll(p[3].c_str()); b.current = p[4] == "*"; b.track = p[5];
    b.type = startsWith(b.ref, "refs/heads") ? "local" : startsWith(b.ref, "refs/tags") ? "tag" : "remote";
    if (endsWith(b.name, "/HEAD") || (b.type == "remote" && b.name.find('/') == std::string::npos)) continue;
    r.push_back(b);
  }
  return r;
}

std::vector<Remote> remotes(const std::string &cwd) {
  std::vector<Remote> r;
  for (auto &l : split(run(cwd, { "remote", "-v" }, "", nullptr, true), '\n')) {
    std::istringstream is(l); std::string name, url, kind;
    if (!(is >> name >> url >> kind)) continue;
    auto it = std::find_if(r.begin(), r.end(), [&](auto &x) { return x.name == name; });
    if (it == r.end()) { r.push_back({ name, "", "" }); it = r.end() - 1; }
    (kind == "(push)" ? it->push : it->fetch) = url;
  }
  return r;
}

bool isRepo(const std::string &cwd) {
  if (!dirExists(cwd)) return false;
  try { return trim(run(cwd, { "rev-parse", "--is-inside-work-tree" }, "", nullptr, true)) == "true"; } catch (...) { return false; }
}
std::string topLevel(const std::string &cwd) { return toBackslash(trim(run(cwd, { "rev-parse", "--show-toplevel" }))); }
long long lastActivity(const std::string &cwd) { return atoll(trim(run(cwd, { "log", "-1", "--all", "--format=%ct" }, "", nullptr, true)).c_str()); }

static std::string gitDir(const std::string &cwd) {
  std::string g = toBackslash(trim(run(cwd, { "rev-parse", "--git-dir" })));
  return isAbsPath(g) ? g : cwd + KG_SEP + g;
}
std::string conflictState(const std::string &cwd) {
  std::string d = gitDir(cwd);
  if (fileExists(d + KG_SEP "MERGE_HEAD")) return "merge";
  if (dirExists(d + KG_SEP "rebase-merge") || dirExists(d + KG_SEP "rebase-apply")) return "rebase";
  if (fileExists(d + KG_SEP "CHERRY_PICK_HEAD")) return "cherry-pick";
  return "";
}

std::vector<Stash> stashList(const std::string &cwd) {
  std::vector<Stash> r;
  for (auto &l : split(trim(run(cwd, { "stash", "list", std::string("--format=%gd") + SEP + "%s" + SEP + "%ct" })), '\n')) {
    Strings p = split(l, SEP, true);
    if (p.size() >= 3) r.push_back({ p[0], p[1], atoll(p[2].c_str()) });
  }
  return r;
}

void init(const std::string &cwd, const std::string &branch) { run(cwd, { "init", "-b", branch }); }
void clone(const std::string &url, const std::string &dest, const Progress &p, int depth) {
  Strings a = { "clone", "--progress" };
  if (depth) a.push_back("--depth=" + std::to_string(depth));
  a.push_back(url); a.push_back(dest);
  makeDirs(dirName(dest));
  run(dirName(dest), a, url, p);
}
void stage(const std::string &cwd, const Strings &files) { Strings a = { "add", "--" }; a.insert(a.end(), files.begin(), files.end()); run(cwd, a); }
void stageAll(const std::string &cwd) { run(cwd, { "add", "-A" }); }
void unstage(const std::string &cwd, const Strings &files) { Strings a = { "reset", "-q", "HEAD", "--" }; a.insert(a.end(), files.begin(), files.end()); run(cwd, a, "", nullptr, true); }
void applyPatch(const std::string &cwd, const std::string &patch, bool cached, bool reverse) {
  Strings a = { "apply" }; if (cached) a.push_back("--cached"); if (reverse) a.push_back("-R");
  a.push_back("--unidiff-zero"); a.push_back("-");
  run(cwd, a, "", nullptr, false, patch);
}
void discard(const std::string &cwd, const Strings &tracked, const Strings &untracked) {
  if (!untracked.empty()) { Strings a = { "clean", "-f", "--" }; a.insert(a.end(), untracked.begin(), untracked.end()); run(cwd, a); }
  if (!tracked.empty()) { Strings a = { "checkout", "HEAD", "--" }; a.insert(a.end(), tracked.begin(), tracked.end()); run(cwd, a); }
}
void commit(const std::string &cwd, const std::string &msg, bool amend) {
  if (msg.empty()) { run(cwd, { "commit", "--amend", "--no-edit" }); return; }
  Strings a = { "commit", "-F", "-" }; if (amend) a.push_back("--amend");
  run(cwd, a, "", nullptr, false, msg);
}
// Splits the staged set into several commits of `size` files each.
void commitChunked(const std::string &cwd, const std::string &message, int size, const Progress &p) {
  Strings staged;
  for (auto &f : status(cwd).files) if (f.staged) staged.push_back(f.path);
  if (staged.empty()) throw std::runtime_error("Nothing staged");
  run(cwd, { "reset", "-q" }, "", nullptr, true);
  int total = ((int)staged.size() + size - 1) / size;
  for (int i = 0; i < total; i++) {
    Strings a = { "add", "-A", "--" };
    for (size_t j = (size_t)i * size; j < staged.size() && j < (size_t)(i + 1) * size; j++) a.push_back(staged[j]);
    run(cwd, a);
    std::string m = total > 1 ? message + "\n\n[part " + std::to_string(i + 1) + "/" + std::to_string(total) + "]" : message;
    run(cwd, { "commit", "-F", "-" }, "", nullptr, false, m);
    if (p) p("Committed part " + std::to_string(i + 1) + "/" + std::to_string(total) + "\n");
  }
}
void undoCommit(const std::string &cwd) { run(cwd, { "reset", "--soft", "HEAD~1" }); }
Strings unpushed(const std::string &cwd) {
  Strings out; std::istringstream is(run(cwd, { "rev-list", "HEAD", "--not", "--remotes", "--max-count=2000" }, "", nullptr, true, ""));
  for (std::string l; std::getline(is, l);) { l = trim(l); if (!l.empty()) out.push_back(l); }
  return out;
}
void checkout(const std::string &cwd, const std::string &n) { run(cwd, { "checkout", n }); }
void checkoutRemote(const std::string &cwd, const std::string &r, const std::string &l) { run(cwd, { "checkout", "-b", l, "--track", r }); }
void createBranch(const std::string &cwd, const std::string &n, const std::string &from) {
  Strings a = { "checkout", "-b", n }; if (!from.empty()) a.push_back(from); run(cwd, a);
}
void renameBranch(const std::string &cwd, const std::string &o, const std::string &n) { run(cwd, { "branch", "-m", o, n }); }
void deleteBranch(const std::string &cwd, const std::string &n, bool force) { run(cwd, { "branch", force ? "-D" : "-d", n }); }
void deleteRemoteBranch(const std::string &cwd, const std::string &remote, const std::string &n) {
  run(cwd, { "push", remote, "--delete", n }, remoteUrl(cwd, remote));
}
void merge(const std::string &cwd, const std::string &n, bool squash) { run(cwd, { "merge", squash ? "--squash" : "--no-edit", n }); }
void rebase(const std::string &cwd, const std::string &onto) { run(cwd, { "rebase", onto }); }
void abort(const std::string &cwd) {
  std::string d = gitDir(cwd);
  if (dirExists(d + KG_SEP "rebase-merge") || dirExists(d + KG_SEP "rebase-apply")) run(cwd, { "rebase", "--abort" });
  else if (fileExists(d + KG_SEP "CHERRY_PICK_HEAD")) run(cwd, { "cherry-pick", "--abort" });
  else if (fileExists(d + KG_SEP "REVERT_HEAD")) run(cwd, { "revert", "--abort" });
  else run(cwd, { "merge", "--abort" });
}
void continueOp(const std::string &cwd, const std::string &kind) {
  // GIT_EDITOR=true so merge/rebase --continue accept the prepared message instead of waiting on an editor.
  Strings a = { "-c", "core.editor=true", kind.empty() ? "merge" : kind, "--continue" };
  run(cwd, a);
}
void cherryPick(const std::string &cwd, const std::string &h) { run(cwd, { "cherry-pick", h }); }
void revert(const std::string &cwd, const std::string &h) { run(cwd, { "revert", "--no-edit", h }); }
void resetTo(const std::string &cwd, const std::string &h, const std::string &mode) { run(cwd, { "reset", "--" + mode, h }); }
void tag(const std::string &cwd, const std::string &n, const std::string &h) { run(cwd, { "tag", n, h }); }
void deleteTag(const std::string &cwd, const std::string &n) { run(cwd, { "tag", "-d", n }); }
void stash(const std::string &cwd, const std::string &m) {
  Strings a = { "stash", "push", "-u" }; if (!m.empty()) { a.push_back("-m"); a.push_back(m); } run(cwd, a);
}
void stashPop(const std::string &cwd, const std::string &r) { run(cwd, { "stash", "pop", r }); }
void stashDrop(const std::string &cwd, const std::string &r) { run(cwd, { "stash", "drop", r }); }
void addRemote(const std::string &cwd, const std::string &n, const std::string &u) { run(cwd, { "remote", "add", n, u }); }
void removeRemote(const std::string &cwd, const std::string &n) { run(cwd, { "remote", "remove", n }); }
void setRemoteUrl(const std::string &cwd, const std::string &n, const std::string &u) { run(cwd, { "remote", "set-url", n, u }); }

void fetch(const std::string &cwd, const std::string &remote, const Progress &p) {
  run(cwd, { "fetch", "--progress", "--prune", remote }, remoteUrl(cwd, remote), p);
}
void fetchAll(const std::string &cwd, const Progress &p) { for (auto &r : remotes(cwd)) fetch(cwd, r.name, p); }
void pull(const std::string &cwd, const std::string &remote, const std::string &branch, bool rebase, const Progress &p) {
  run(cwd, { "pull", "--progress", rebase ? "--rebase" : "--no-rebase", remote, branch }, remoteUrl(cwd, remote), p);
}
void push(const std::string &cwd, const std::string &remote, const std::string &branch, const PushOpts &o, const Progress &p) {
  Strings a = { "push", "--progress" };
  if (o.setUpstream) a.push_back("-u");
  if (o.force) a.push_back("--force-with-lease");
  if (o.tags) a.push_back("--follow-tags");
  a.push_back(remote); a.push_back(branch);
  run(cwd, a, remoteUrl(cwd, remote), p);
}

template <class F> static void retry(F fn, const Progress &p, int tries = 5) {
  for (int i = 1;; i++) {
    try { fn(); return; } catch (std::exception &e) {
      if (i >= tries) throw;
      if (p) p("Attempt " + std::to_string(i) + " failed (" + split(e.what(), '\n')[0] + "), retrying…\n");
      sleepMs(1500 * i);
    }
  }
}

// Pushes unpushed commits a few at a time so a dropped connection loses at most one batch.
void pushChunked(const std::string &cwd, const std::string &remote, const std::string &branch, int size, const Progress &p) {
  std::string url = remoteUrl(cwd, remote), remoteRef = remote + "/" + branch;
  bool has = !trim(run(cwd, { "rev-parse", "--verify", "-q", remoteRef }, "", nullptr, true)).empty();
  Strings list = split(trim(run(cwd, { "rev-list", "--reverse", "--first-parent", has ? remoteRef + "..HEAD" : "HEAD" })), '\n');
  if (list.empty()) { if (p) p("Nothing to push\n"); return; }
  Strings steps;
  for (size_t i = size - 1; i < list.size(); i += size) steps.push_back(list[i]);
  if (steps.empty() || steps.back() != list.back()) steps.push_back(list.back());
  for (size_t i = 0; i < steps.size(); i++) {
    retry([&] { run(cwd, { "push", "--progress", remote, steps[i] + ":refs/heads/" + branch }, url, p); }, p);
    if (p) p("Pushed chunk " + std::to_string(i + 1) + "/" + std::to_string(steps.size()) + "\n");
  }
  run(cwd, { "branch", "--set-upstream-to=" + remoteRef }, "", nullptr, true);
}

// Fetches history in depth increments (shallow repos) or bounded steps with retries, then merges.
void pullChunked(const std::string &cwd, const std::string &remote, const std::string &branch, int size, const Progress &p) {
  std::string url = remoteUrl(cwd, remote);
  bool shallow = trim(run(cwd, { "rev-parse", "--is-shallow-repository" })) == "true";
  Strings cfg = { "-c", "http.lowSpeedLimit=1000", "-c", "http.lowSpeedTime=60", "-c", "http.postBuffer=524288000" };
  auto with = [&](Strings a) { Strings r = cfg; r.insert(r.end(), a.begin(), a.end()); return r; };
  if (shallow) {
    for (int i = 1;; i++) {
      std::string before = trim(run(cwd, { "rev-list", "--count", "--all" }));
      retry([&] { run(cwd, with({ "fetch", "--progress", "--deepen=" + std::to_string(size), remote, branch }), url, p); }, p);
      std::string after = trim(run(cwd, { "rev-list", "--count", "--all" }));
      if (p) p("Deepened round " + std::to_string(i) + ": " + after + " commits\n");
      if (before == after) break;
    }
  } else {
    retry([&] { run(cwd, with({ "fetch", "--progress", remote, branch }), url, p); }, p);
  }
  run(cwd, { "merge", "--no-edit", remote + "/" + branch });
}
void unshallow(const std::string &cwd, const std::string &remote, const Progress &p) {
  run(cwd, { "fetch", "--progress", "--unshallow", remote }, remoteUrl(cwd, remote), p);
}
std::string config(const std::string &cwd, const std::string &key) {
  Strings a = { "config" }; if (cwd.empty()) a.push_back("--global");
  a.push_back("--get"); a.push_back(key);
  return trim(run(cwd.empty() ? homeDir() : cwd, a, "", nullptr, true));
}
void setConfig(const std::string &cwd, const std::string &key, const std::string &val) {
  Strings a = { "config" }; if (cwd.empty()) a.push_back("--global");
  a.push_back(key); a.push_back(val);
  run(cwd.empty() ? homeDir() : cwd, a);
}

}  // namespace git
