#include "ssh.h"
#include <windows.h>
#include <algorithm>
#include <regex>

namespace ssh {

static std::string sysDir() {
  wchar_t buf[MAX_PATH]; GetEnvironmentVariableW(L"SystemRoot", buf, MAX_PATH);
  std::string root = narrow(buf); if (root.empty()) root = "C:\\Windows";
  return root + "\\System32\\OpenSSH";
}
std::string exe(const std::string &name) {
  std::string p = sysDir() + "\\" + name + ".exe";
  return fileExists(p) ? p : name;
}
std::string sshDir() { return homeDir() + "\\.ssh"; }
bool available() { return fileExists(sysDir() + "\\ssh.exe"); }

static std::string run(const std::string &file, const Strings &args, bool throwOnFail = true) {
  ProcResult r = runProcess(file, args);
  std::string all = trim(r.out + r.err);
  if (r.code != 0 && throwOnFail) throw std::runtime_error(trim(r.err).empty() ? "Command failed" : trim(r.err));
  return all;
}

std::vector<Key> list(bool fp) {
  std::vector<Key> out;
  std::string dir = sshDir();
  if (!dirExists(dir)) return out;
  for (auto &f : listDir(dir)) {
    if (!endsWith(f, ".pub")) continue;
    Key k;
    k.name = f.substr(0, f.size() - 4);
    k.pubPath = dir + "\\" + f;
    k.path = dir + "\\" + k.name;
    k.hasPrivate = fileExists(k.path);
    try {
      k.publicKey = trim(readFile(k.pubPath));
      auto parts = split(k.publicKey, ' ');
      if (!parts.empty()) k.type = parts[0];
      if (parts.size() > 2) k.comment = join(Strings(parts.begin() + 2, parts.end()), " ");
    } catch (...) {}
    if (fp) {
      try { auto p = split(run(exe("ssh-keygen"), { "-lf", k.pubPath }), ' '); if (p.size() > 1) k.fingerprint = p[1]; } catch (...) {}
    }
    out.push_back(k);
  }
  return out;
}

Strings privateKeys() {
  Strings r;
  for (auto &k : list(false)) if (k.hasPrivate) r.push_back(k.path);
  return r;
}

std::vector<Key> scanFolder(const std::string &dir) {
  std::vector<Key> keys;
  if (!dirExists(dir)) return keys;
  Strings files = listDir(dir);
  std::vector<std::string> seen;
  for (auto &f : files) {
    if (!endsWith(f, ".pub")) continue;
    Key k; k.name = f.substr(0, f.size() - 4); seen.push_back(k.name);
    k.path = dir + "\\" + k.name; k.pubPath = dir + "\\" + f; k.hasPrivate = fileExists(k.path);
    try {
      auto parts = split(trim(readFile(k.pubPath)), ' ');
      if (!parts.empty()) k.type = parts[0];
      if (parts.size() > 2) k.comment = join(Strings(parts.begin() + 2, parts.end()), " ");
    } catch (...) {}
    keys.push_back(k);
  }
  for (auto &f : files) {
    if (endsWith(f, ".pub") || startsWith(f, "known_hosts") || startsWith(f, "config")) continue;
    if (std::find(seen.begin(), seen.end(), f) != seen.end()) continue;
    std::string p = dir + "\\" + f;
    if (!fileExists(p)) continue;
    try {
      std::string head = readFile(p).substr(0, 100);
      if (head.find("BEGIN") != std::string::npos && head.find("PRIVATE KEY") != std::string::npos) {
        Key k; k.name = f; k.path = p; k.hasPrivate = true; k.type = "private key"; keys.push_back(k);
      }
    } catch (...) {}
  }
  return keys;
}

std::string generate(const std::string &name, const std::string &type, const std::string &comment, const std::string &pass) {
  if (!std::regex_match(name, std::regex("^[\\w.-]+$"))) throw std::runtime_error("Key name may only contain letters, digits, . _ -");
  makeDirs(sshDir());
  std::string p = sshDir() + "\\" + name;
  if (fileExists(p)) throw std::runtime_error("A key with that name already exists");
  Strings args = { "-t", type, "-f", p, "-N", pass, "-C", comment, "-q" };
  if (type == "rsa") { args.push_back("-b"); args.push_back("4096"); }
  run(exe("ssh-keygen"), args);
  return p;
}
void setComment(const std::string &k, const std::string &c, const std::string &p) { run(exe("ssh-keygen"), { "-c", "-C", c, "-P", p, "-f", k }); }
void changePassphrase(const std::string &k, const std::string &o, const std::string &n) { run(exe("ssh-keygen"), { "-p", "-P", o, "-N", n, "-f", k }); }
std::string rename(const std::string &k, const std::string &newName) {
  std::string np = sshDir() + "\\" + newName;
  if (fileExists(np)) throw std::runtime_error("Name already in use");
  if (!MoveFileW(widen(k).c_str(), widen(np).c_str())) throw std::runtime_error("Rename failed");
  if (fileExists(k + ".pub")) MoveFileW(widen(k + ".pub").c_str(), widen(np + ".pub").c_str());
  return np;
}
void remove(const std::string &k) {
  DeleteFileW(widen(k).c_str()); DeleteFileW(widen(k + ".pub").c_str());
}
std::string import(const std::string &name, const std::string &priv) {
  makeDirs(sshDir());
  std::string p = sshDir() + "\\" + name;
  std::string body = priv; size_t i;
  while ((i = body.find("\r\n")) != std::string::npos) body.erase(i, 1);
  writeFile(p, trim(body) + "\n");
  std::string pub = run(exe("ssh-keygen"), { "-y", "-f", p });
  writeFile(p + ".pub", pub + " " + name + "\n");
  return p;
}
std::string addToAgent(const std::string &k) { return run(exe("ssh-add"), { k }); }
std::string agentList() { return run(exe("ssh-add"), { "-l" }, false); }
// `ssh -T` exits non-zero even when auth succeeds (no shell), so return the output either way.
std::string test(const std::string &host, const std::string &key) {
  Strings args = { "-T", "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10" };
  if (!key.empty()) { args.insert(args.end(), { "-i", key, "-o", "IdentitiesOnly=yes" }); }
  args.push_back("git@" + host);
  try { return run(exe("ssh"), args, false); } catch (std::exception &e) { return e.what(); }
}

}  // namespace ssh
