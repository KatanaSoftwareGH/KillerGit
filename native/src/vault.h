#pragma once
#include "util.h"
#include <mutex>

// Encrypted store for accounts (tokens, assigned SSH keys) and free-form secrets, plus plain app settings.
// Secrets: %APPDATA%\KillerGit\vault.dat, encrypted with DPAPI (CryptProtectData, current user).
// On first run it migrates the Electron app's vault.bin (Chromium OSCrypt AES-GCM, key in "Local State").
namespace vault {

struct Preset { const char *id, *name, *host, *api; };
extern const Preset PRESETS[4];  // github, gitlab, gitgud, custom
const Preset *preset(const std::string &id);

json accounts();                                  // full objects including token
json account(const std::string &id);              // null if missing
json accountForHost(const std::string &host);     // matches subdomains too
std::string saveAccount(json acct);               // returns id; empty token keeps the old one
void removeAccount(const std::string &id);

json secrets();
void saveSecret(json s);
void removeSecret(const std::string &id);

json settings();
json saveSettings(const json &patch);             // shallow merge, returns merged

}  // namespace vault
