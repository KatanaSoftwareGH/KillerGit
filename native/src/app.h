#pragma once
#include "git.h"
#include "providers.h"
#include "ssh.h"
#include "util.h"
#include "vault.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <atomic>
#include <memory>
#include <optional>

Strings githubDesktopRepos();

// ---------------- threading ----------------
// bg() runs work on a worker thread; ui() queues a closure for the UI thread (and wakes it).
void bg(std::function<void()> work);
void ui(std::function<void()> fn);
void drainUiQueue();
void wakeUi();

// ---------------- theme ----------------
struct Palette {
  ImVec4 bg, side, card, cardHover, border, text, muted, faint, accent, accentHover, accentText,
         danger, add, del, addBg, delBg, hunkBg, sel, input;
};
extern Palette C;
struct Fonts { ImFont *body, *bold, *small, *title, *heading, *headingLarge, *mono; };
extern Fonts F;
extern ImTextureID g_appIcon;   // app icon texture (0 if it failed to load)
void AppIcon(float size);       // draws the icon inline, vertically centred on the current line
void applyTheme(bool dark, ImVec4 accent);
void buildFonts(float scale);
bool systemPrefersDark();
ImU32 col(const ImVec4 &c, float alpha = 1.f);
ImVec4 hexColor(const std::string &hex);
std::string toHex(const ImVec4 &c);

// ---------------- widgets ----------------
bool Button(const char *label, bool primary = false, bool danger = false, ImVec2 size = {0, 0});
bool IconButton(const char *icon, const char *tooltip);
bool ToolbarItem(const char *id, const char *small, const std::string &value, float width, const char *tooltip = nullptr);
void Heading(const char *text, bool large = false);
void Muted(const std::string &text);
void Spinner(float radius, float thickness);
bool Pill(const char *label, bool active);
void Badge(char code);
void RefChip(const std::string &text, ImVec4 color);
void BeginCard(const char *id);
void EndCard();
void Empty(const char *title, const std::string &sub);
bool SelectableRow(const char *id, bool selected, float height);

// ---------------- app state ----------------
enum class Tab { Changes, History, Branches, Stashes, Remotes, Issues };

struct RepoEntry {
  std::string path, name, host, ns;
  long long lastActivity = 0;
  int changed = 0, ahead = 0, behind = 0;
  bool missing = false, pending = true;
};

struct GraphRow { int col = 0; std::vector<std::string> before, after, parents; bool merge = false; };

struct DiffLine { char kind; int oldNo, newNo; std::string text; int hunk; };
struct DiffHunk { std::string header, patch; };
struct ParsedDiff { std::string title, kind; std::vector<DiffLine> lines; std::vector<DiffHunk> hunks; bool tooLarge = false; };
ParsedDiff parseDiff(const std::string &text, const std::string &title, const std::string &kind);

struct Modal {
  std::string title;
  std::function<bool()> draw;   // return false to close
  float width = 480;
  bool opened = false;
};

struct Toast { std::string text; bool error; double until; };

struct App {
  json settings;
  std::vector<RepoEntry> repos;
  bool reposLoading = false;
  int reposRun = 0;
  std::string filter;

  std::string repo;
  std::optional<git::Status> status;
  std::vector<git::Remote> remotes;
  std::string conflict;
  bool repoLoading = false;
  int repoGen = 0;
  Tab tab = Tab::Changes;

  // changes
  std::string selFile, draftSummary, draftDesc;
  bool amend = false;
  std::vector<ParsedDiff> fileDiffs; bool diffLoading = false; std::string diffFor; int diffGen = 0;

  // history
  std::vector<git::Commit> commits; std::vector<GraphRow> rows; std::vector<std::string> lanes;
  bool historyLoaded = false, historyLoading = false, historyMore = false; std::string historyFilter;
  std::string selCommit; std::optional<git::CommitDetail> commitDetail; std::string commitFile;
  std::vector<ParsedDiff> commitDiff; int commitGen = 0;

  // branches / stashes
  std::vector<git::Branch> branchList; bool branchesLoaded = false;
  std::vector<git::Stash> stashes; bool stashesLoaded = false;

  // issues
  int issueRemote = 0; int issueState = 0; std::string issueFilter;
  std::vector<providers::Issue> issues; bool issuesLoading = false; std::string issuesError; bool issuesLoaded = false;
  std::optional<providers::Issue> selIssue; std::vector<providers::Comment> issueComments; std::string issueReply;

  // busy task + progress
  bool busy = false;
  std::string statusText = "Ready", progressLabel, progressChunk, log;
  float progressPct = -1;   // <0 = indeterminate
  std::vector<std::unique_ptr<Modal>> modals;
  std::vector<Toast> toasts;
  float sidebarW = 280;
  float paneW = 340;
  bool dark = false;

  void boot();
  void frame();

  // actions
  void loadRepos();
  void refreshIndicators();
  void openRepo(const std::string &path);
  void refreshRepo();
  void invalidateViews();
  void task(const std::string &label, std::function<void(const Progress &)> fn, bool refresh = true);
  void toast(const std::string &text, bool error = false);
  void saveSettings(const json &patch);
  void setStatus(const std::string &s) { statusText = s; }
  std::string currentRemote() const;
  void addRepoPath(const std::string &dir, bool open = true);

  // sync
  void syncPrimary();
  void doPull(const std::string &remote, int rebase = -1);
  void doPush(const std::string &remote, git::PushOpts o = {});
  void syncMenuItems();
  bool chunkOn() const { return settings.value("chunked", false); }
  int chunkSize() const { int n = settings.value("chunkSize", 50); return n > 0 ? n : 50; }

  // modals
  void modal(const std::string &title, std::function<bool()> draw, float width = 480);
  void confirm(const std::string &title, const std::string &text, const std::string &ok, bool danger, std::function<void()> onOk);
  void prompt(const std::string &title, const std::string &label, const std::string &init, std::function<void(std::string)> onOk);
  void info(const std::string &title, const std::string &text);
  void drawModals(size_t i = 0);

  // views (views.cpp)
  void drawToolbar();
  void drawSidebar();
  void drawMain();
  void drawStatusbar();
  void drawToasts();
  void viewWelcome();
  void viewChanges();
  void viewHistory();
  void viewBranches();
  void viewStashes();
  void viewRemotes();
  void viewIssues();
  void drawDiff(const ParsedDiff &d, const char *id);
  void loadFileDiff(const git::FileStatus &f);
  void loadHistory(bool more);
  void showCommit(const std::string &hash);
  void loadCommitFile(const std::string &file);
  void loadIssues();
  void openIssue(const providers::Issue &i);
  void branchPopup();
  void switchBranch(const git::Branch &b, bool remote);
  void newBranch(const std::string &from);
  void discardFiles(const std::vector<git::FileStatus> &files);
  void commitContextMenu(const git::Commit &c);

  // dialogs (dialogs.cpp)
  void cloneDialog();
  void addLocal();
  void createDialog();
  void publishDialog(bool forLocal, bool cloneAfter);
  void importGithubDesktop();
  void accountsPage();
  void settingsPage();
  void ensureIdentity(std::function<void()> then);
  void addRemoteDialog();
  void cloneTo(const std::string &url, const std::string &dir, int depth);
};

extern App app;
extern std::atomic<bool> g_dirty;
double now();
