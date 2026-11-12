#pragma once
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---- platform ----
#ifdef _WIN32
#define KG_SEP "\\"                      // path separator, as a string literal
#define SHOW_IN_FM "Show in Explorer"
#else
#define KG_SEP "/"
#define SHOW_IN_FM "Show in file manager"
#endif
#include <ctime>
bool isAbsPath(const std::string &p);
void localTime(std::time_t t, std::tm &out);
long long timegm_portable(std::tm *tm);
void sleepMs(int ms);
// Files embedded in the binary (101/102 = Work Sans fonts, 103 = icon PNG). Data stays valid for the process lifetime.
bool embeddedResource(int id, const void *&data, size_t &size);
using Strings = std::vector<std::string>;
using Progress = std::function<void(const std::string &)>;

// ---- strings ----
std::wstring widen(const std::string &s);
std::string narrow(const std::wstring &s);
std::string trim(const std::string &s);
Strings split(const std::string &s, char sep, bool keepEmpty = false);
bool startsWith(const std::string &s, const std::string &p);
bool endsWith(const std::string &s, const std::string &p);
std::string lower(std::string s);
bool icontains(const std::string &hay, const std::string &needle);
std::string join(const Strings &v, const std::string &sep);
std::string baseName(const std::string &path);
std::string dirName(const std::string &path);
std::string toBackslash(std::string p);
std::string ago(long long unixSeconds);
std::string formatTime(long long unixSeconds);
std::string base64(const std::string &bytes);
std::string unbase64(const std::string &text);
std::string urlEncode(const std::string &s);
// Provider host for a git remote host: drops SSH-only front ends like ssh.gitgud.io / altssh.gitlab.com,
// so they group with and use the same account as the web host. Lower-cased.
std::string canonicalHost(const std::string &host);

// ---- processes ----
struct ProcResult { int code = -1; std::string out, err; };
struct ProcOptions {
  std::string cwd;
  std::map<std::string, std::string> env;   // extra/overridden environment variables
  std::string input;                         // written to stdin then closed
  Progress onStderr;                         // streamed stderr chunks
};
ProcResult runProcess(const std::string &exe, const Strings &args, const ProcOptions &opt = {});
void launchDetached(const std::string &cmdline, const std::string &cwd = "");

// ---- files ----
bool fileExists(const std::string &p);
bool dirExists(const std::string &p);
std::string readFile(const std::string &p);            // throws on failure
void writeFile(const std::string &p, const std::string &data);
void appendFile(const std::string &p, const std::string &data);
void makeDirs(const std::string &p);
Strings listDir(const std::string &p);                 // names only
std::string appDataDir();                              // %APPDATA%\KillerGit
std::string homeDir();
std::string documentsDir();
std::string roamingDir();                              // %APPDATA%

// ---- shell ----
void openPath(const std::string &p);
void showInExplorer(const std::string &p);
void openUrl(const std::string &u);
void openTerminal(const std::string &dir);
void openEditor(const std::string &editor, const std::string &dir);
void copyToClipboard(const std::string &s);
std::string pickFolder(const std::string &title);
std::string pickFile(const std::string &title);
