#include "vault.h"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <chrono>

namespace vault {

const Preset PRESETS[4] = {
  { "github", "GitHub.com", "github.com", "https://api.github.com" },
  { "gitlab", "GitLab.com", "gitlab.com", "https://gitlab.com/api/v4" },
  { "gitgud", "Gitgud.io", "gitgud.io", "https://gitgud.io/api/v4" },
  { "custom", "Other GitLab host", "", "" },
};
const Preset *preset(const std::string &id) {
  for (auto &p : PRESETS) if (id == p.id) return &p;
  return nullptr;
}

static std::recursive_mutex mu;
static json data;
static bool loaded = false;

static std::string vaultFile() { return appDataDir() + "\\vault.dat"; }
static std::string settingsFile() { return appDataDir() + "\\settings.json"; }

static std::string dpapi(const std::string &in, bool protect) {
  DATA_BLOB src{ (DWORD)in.size(), (BYTE *)in.data() }, dst{};
  BOOL ok = protect ? CryptProtectData(&src, L"KillerGit", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst)
                    : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst);
  if (!ok) throw std::runtime_error("DPAPI failed");
  std::string r((char *)dst.pbData, dst.cbData); LocalFree(dst.pbData); return r;
}

static std::string aesGcmDecrypt(const std::string &key, const std::string &nonce, const std::string &cipherAndTag) {
  if (cipherAndTag.size() < 16) throw std::runtime_error("bad ciphertext");
  BCRYPT_ALG_HANDLE alg; BCRYPT_KEY_HANDLE hk;
  BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
  BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
  BCryptGenerateSymmetricKey(alg, &hk, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0);
  std::string ct = cipherAndTag.substr(0, cipherAndTag.size() - 16), tag = cipherAndTag.substr(cipherAndTag.size() - 16);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = (PUCHAR)nonce.data(); info.cbNonce = (ULONG)nonce.size();
  info.pbTag = (PUCHAR)tag.data(); info.cbTag = (ULONG)tag.size();
  std::string out(ct.size(), 0); ULONG n = 0;
  NTSTATUS st = BCryptDecrypt(hk, (PUCHAR)ct.data(), (ULONG)ct.size(), &info, nullptr, 0, (PUCHAR)out.data(), (ULONG)out.size(), &n, 0);
  BCryptDestroyKey(hk); BCryptCloseAlgorithmProvider(alg, 0);
  if (st != 0) throw std::runtime_error("AES-GCM decrypt failed");
  out.resize(n); return out;
}

// Electron safeStorage on Windows = Chromium OSCrypt: "v10" + 12-byte nonce + AES-256-GCM(ciphertext+tag),
// key = DPAPI-unprotect(base64(Local State os_crypt.encrypted_key) minus "DPAPI" prefix).
static json migrateElectron() {
  std::string dir = appDataDir();
  std::string raw = readFile(dir + "\\vault.bin");
  if (startsWith(raw, "v10")) {
    json ls = json::parse(readFile(dir + "\\Local State"));
    std::string k = unbase64(ls["os_crypt"]["encrypted_key"].get<std::string>());
    if (!startsWith(k, "DPAPI")) throw std::runtime_error("unknown key format");
    std::string key = dpapi(k.substr(5), false);
    raw = aesGcmDecrypt(key, raw.substr(3, 12), raw.substr(15));
  }
  return json::parse(raw);
}

static void save() {
  writeFile(vaultFile(), dpapi(data.dump(), true));
}

static json &load() {
  std::lock_guard l(mu);
  if (loaded) return data;
  loaded = true;
  try { data = json::parse(dpapi(readFile(vaultFile()), false)); }
  catch (...) {
    data = json::object();
    if (fileExists(appDataDir() + "\\vault.bin")) {
      try { data = migrateElectron(); save(); } catch (...) {}
    }
  }
  if (!data.contains("accounts") || !data["accounts"].is_array()) data["accounts"] = json::array();
  if (!data.contains("secrets") || !data["secrets"].is_array()) data["secrets"] = json::array();
  return data;
}

static std::string newId() {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  static const char *d = "0123456789abcdefghijklmnopqrstuvwxyz"; std::string s;
  while (ms) { s.insert(s.begin(), d[ms % 36]); ms /= 36; }
  return s;
}

json accounts() { std::lock_guard l(mu); return load()["accounts"]; }
json account(const std::string &id) {
  std::lock_guard l(mu);
  for (auto &a : load()["accounts"]) if (a.value("id", "") == id) return a;
  return nullptr;
}
json accountForHost(const std::string &host) {
  std::lock_guard l(mu);
  auto &as = load()["accounts"];
  for (auto &a : as) if (a.value("host", "") == host) return a;
  for (auto &a : as) { std::string h = a.value("host", ""); if (!h.empty() && endsWith(host, "." + h)) return a; }
  return nullptr;
}
std::string saveAccount(json in) {
  std::lock_guard l(mu);
  auto &as = load()["accounts"];
  std::string id = in.value("id", "");
  if (id.empty()) id = newId();
  json old = nullptr;
  for (auto &a : as) if (a.value("id", "") == id) old = a;
  auto pick = [&](const char *k) { std::string v = in.value(k, ""); return !v.empty() || in.contains(k) ? v : (old.is_object() ? old.value(k, "") : ""); };
  std::string provider = pick("provider");
  const Preset *p = preset(provider);
  json a;
  a["id"] = id;
  a["provider"] = provider;
  a["kind"] = provider == "github" ? "github" : "gitlab";
  std::string host = pick("host"); if (host.empty() && p) host = p->host;
  a["host"] = lower(host);
  std::string api = pick("api"); if (api.empty()) api = p && *p->api ? p->api : "https://" + host + "/api/v4";
  a["api"] = api;
  a["username"] = pick("username");
  a["sshKey"] = pick("sshKey");
  a["label"] = pick("label");
  std::string token = in.value("token", "");
  a["token"] = !token.empty() ? token : (old.is_object() ? old.value("token", "") : "");
  json next = json::array();
  for (auto &x : as) if (x.value("id", "") != id) next.push_back(x);
  next.push_back(a);
  as = next;
  save();
  return id;
}
void removeAccount(const std::string &id) {
  std::lock_guard l(mu);
  auto &as = load()["accounts"]; json next = json::array();
  for (auto &x : as) if (x.value("id", "") != id) next.push_back(x);
  as = next; save();
}
json secrets() { std::lock_guard l(mu); return load()["secrets"]; }
void saveSecret(json s) {
  std::lock_guard l(mu);
  auto &ss = load()["secrets"];
  std::string id = s.value("id", ""); if (id.empty()) id = newId();
  std::string value = s.value("value", "");
  json next = json::array();
  for (auto &x : ss) { if (x.value("id", "") == id) { if (value.empty()) value = x.value("value", ""); } else next.push_back(x); }
  next.push_back({ { "id", id }, { "name", s.value("name", "") }, { "kind", s.value("kind", "token") }, { "value", value } });
  ss = next; save();
}
void removeSecret(const std::string &id) {
  std::lock_guard l(mu);
  auto &ss = load()["secrets"]; json next = json::array();
  for (auto &x : ss) if (x.value("id", "") != id) next.push_back(x);
  ss = next; save();
}

static json settingsCache;
static bool settingsLoaded = false;
json settings() {
  std::lock_guard l(mu);
  if (!settingsLoaded) {
    settingsLoaded = true;
    try { settingsCache = json::parse(readFile(settingsFile())); } catch (...) { settingsCache = json::object(); }
    if (!settingsCache.is_object()) settingsCache = json::object();
  }
  return settingsCache;
}
json saveSettings(const json &patch) {
  std::lock_guard l(mu);
  settings();
  for (auto &[k, v] : patch.items()) settingsCache[k] = v;
  try { writeFile(settingsFile(), settingsCache.dump(2)); } catch (...) {}
  return settingsCache;
}

}  // namespace vault
