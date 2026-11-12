#include "app.h"
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#endif
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <regex>
#include <thread>

App app;
std::atomic<bool> g_dirty{ true };
void platformWake();  // main_win.cpp / main_linux.cpp: nudge the UI loop out of its idle wait

double now() { return ImGui::GetTime(); }

// ---------------- threading ----------------
static std::mutex qmu;
static std::condition_variable qcv;
static std::deque<std::function<void()>> work;
static std::vector<std::function<void()>> uiq;
static std::mutex uimu;
static bool poolStarted = false;

void bg(std::function<void()> fn) {
  if (!poolStarted) {
    poolStarted = true;
    for (int i = 0; i < 8; i++) std::thread([] {
#ifdef _WIN32
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
      for (;;) {
        std::function<void()> f;
        { std::unique_lock l(qmu); qcv.wait(l, [] { return !work.empty(); }); f = std::move(work.front()); work.pop_front(); }
        try { f(); } catch (...) {}
      }
    }).detach();
  }
  { std::lock_guard l(qmu); work.push_back(std::move(fn)); }
  qcv.notify_one();
}
void wakeUi() { g_dirty = true; platformWake(); }
void ui(std::function<void()> fn) { { std::lock_guard l(uimu); uiq.push_back(std::move(fn)); } wakeUi(); }
void drainUiQueue() {
  std::vector<std::function<void()>> q;
  { std::lock_guard l(uimu); q.swap(uiq); }
  for (auto &f : q) f();
}

// ---------------- settings / toasts / modals ----------------
void App::saveSettings(const json &patch) {
  settings = vault::saveSettings(patch);
  std::string t = settings.value("theme", "system");
  dark = t == "dark" || (t == "system" && systemPrefersDark());
  applyTheme(dark, hexColor(settings.value("uiAccent", "#d97757")));
}
void App::toast(const std::string &text, bool error) { toasts.push_back({ text, error, now() + (error ? 8 : 4) }); }

void App::modal(const std::string &title, std::function<bool()> draw, float width) {
  auto m = std::make_unique<Modal>();
  m->title = title + "##m" + std::to_string(modals.size()) + "_" + std::to_string((long long)(now() * 1000));
  m->draw = std::move(draw); m->width = width;
  modals.push_back(std::move(m));
}
void App::confirm(const std::string &title, const std::string &text, const std::string &ok, bool danger, std::function<void()> onOk) {
  modal(title, [=] {
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::Spacing();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 200);
    if (Button("Cancel", false, false, ImVec2(92, 0))) return false;
    ImGui::SameLine();
    if (Button(ok.c_str(), true, danger, ImVec2(100, 0))) { onOk(); return false; }
    return true;
  }, 440);
}
void App::prompt(const std::string &title, const std::string &label, const std::string &init, std::function<void(std::string)> onOk) {
  auto val = std::make_shared<std::string>(init);
  auto first = std::make_shared<bool>(true);
  modal(title, [=] {
    ImGui::TextUnformatted(label.c_str());
    if (*first) { ImGui::SetKeyboardFocusHere(); *first = false; }
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##v", val.get(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 200);
    if (Button("Cancel", false, false, ImVec2(92, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) return false;
    ImGui::SameLine();
    if (Button("OK", true, false, ImVec2(100, 0)) || enter) { if (!trim(*val).empty()) onOk(trim(*val)); return false; }
    return true;
  }, 440);
}
void App::info(const std::string &title, const std::string &text) {
  auto t = std::make_shared<std::string>(text);
  modal(title, [=] {
    ImGui::PushFont(F.mono);
    ImGui::InputTextMultiline("##t", t.get(), ImVec2(-1, std::min(320.f, ImGui::CalcTextSize(t->c_str(), nullptr, false, 560).y + 24)), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
    if (Button("Copy")) copyToClipboard(*t);
    ImGui::SameLine();
    return !Button("OK", true);
  }, 600);
}

void App::drawModals(size_t i) {
  if (i >= modals.size()) return;
  Modal &m = *modals[i];
  if (!m.opened) { ImGui::OpenPopup(m.title.c_str()); m.opened = true; }
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y * 0.42f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(m.width, 0), ImVec2(std::min(m.width, vp->WorkSize.x - 40), vp->WorkSize.y - 60));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, C.bg);
  ImGui::PushFont(F.heading);
  bool open = true;
  bool visible = ImGui::BeginPopupModal(m.title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  if (!visible) { modals.erase(modals.begin() + i); return; }
  bool keep = open && m.draw();
  // Nested modals open on top of this one.
  if (keep && i + 1 < modals.size()) drawModals(i + 1);
  if (!keep) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); modals.erase(modals.begin() + i); return; }
  ImGui::EndPopup();
}

// ---------------- repo list ----------------
std::string App::currentRemote() const {
  std::string pref;
  if (settings.contains("repoRemote") && settings["repoRemote"].is_object()) pref = settings["repoRemote"].value(repo, "");
  for (auto &r : remotes) if (r.name == pref) return pref;
  for (auto &r : remotes) if (r.name == "origin") return "origin";
  return remotes.empty() ? "" : remotes[0].name;
}

void App::loadRepos() {
  int run = ++reposRun;
  repos.clear();
  Strings paths = settings.value("repos", Strings{});
  for (auto &p : paths) { RepoEntry e; e.path = p; e.name = baseName(p); repos.push_back(e); }
  reposLoading = !paths.empty();
  auto remaining = std::make_shared<std::atomic<int>>((int)paths.size());
  for (auto &p : paths) {
    bg([=] {
      RepoEntry e; e.path = p; e.name = baseName(p); e.pending = false;
      if (!git::isRepo(p)) e.missing = true;
      else {
        try {
          auto rs = git::remotes(p);
          const git::Remote *primary = nullptr;
          for (auto &r : rs) if (r.name == "origin") primary = &r;
          if (!primary && !rs.empty()) primary = &rs[0];
          providers::Parsed parsed;
          if (primary && providers::parseRemote(primary->fetch, parsed)) { e.host = parsed.host; e.ns = parsed.ns; }
          e.lastActivity = git::lastActivity(p);
        } catch (...) {}
      }
      bool last = --*remaining == 0;
      ui([=] {
        if (run != reposRun) return;
        for (auto &r : repos) if (r.path == p) { int c = r.changed, a = r.ahead, b = r.behind; r = e; r.changed = c; r.ahead = a; r.behind = b; }
        if (last) { reposLoading = false; refreshIndicators(); }
      });
    });
  }
}

// Change dots are checked separately: `git status` can be slow in big repos and must not block the list.
void App::refreshIndicators() {
  int run = reposRun;
  for (auto &r : repos) {
    if (r.missing || r.path == repo) continue;
    std::string p = r.path;
    bg([=] {
      try {
        auto st = git::status(p, "normal");
        ui([=] {
          if (run != reposRun) return;
          for (auto &x : repos) if (x.path == p && x.path != repo) { x.changed = (int)st.files.size(); x.ahead = st.ahead; x.behind = st.behind; }
        });
      } catch (...) {}
    });
  }
}

void App::addRepoPath(const std::string &dir, bool open) {
  bg([=] {
    try {
      std::string top = git::topLevel(dir);
      ui([=] {
        Strings list = settings.value("repos", Strings{});
        if (std::find(list.begin(), list.end(), top) == list.end()) list.push_back(top);
        saveSettings({ { "repos", list } });
        loadRepos();
        if (open) openRepo(top);
      });
    } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); }
  });
}

void App::invalidateViews() {
  historyLoaded = false; commits.clear(); rows.clear(); lanes.clear();
  branchesLoaded = false; stashesLoaded = false;
  fileDiffs.clear(); diffFor.clear();
}

void App::openRepo(const std::string &p) {
  repo = p;
  selFile.clear(); selCommit.clear(); commitDetail.reset(); commitDiff.clear();
  status.reset(); remotes.clear(); conflict.clear();
  issuesLoaded = false; issues.clear(); selIssue.reset(); issueRemote = 0;
  invalidateViews();
  repoLoading = true;
  saveSettings({ { "lastRepo", p } });
  refreshRepo();
}

void App::refreshRepo() {
  if (repo.empty()) return;
  std::string r = repo; int gen = ++repoGen;
  bg([=] {
    std::optional<git::Status> st; std::vector<git::Remote> rs; std::string cf, err;
    try { st = git::status(r); rs = git::remotes(r); cf = git::conflictState(r); }
    catch (std::exception &e) { err = e.what(); }
    ui([=] {
      if (repo != r || gen != repoGen) return;
      repoLoading = false;
      status = st; remotes = rs; conflict = cf;
      if (!err.empty()) toast(err, true);
      for (auto &e : repos) if (e.path == r) { e.changed = st ? (int)st->files.size() : 0; e.ahead = st ? st->ahead : 0; e.behind = st ? st->behind : 0; }
      // Views that show repository state reload lazily.
      branchesLoaded = false; stashesLoaded = false; historyLoaded = false;
      diffFor.clear();
    });
  });
}

// ---------------- busy task with progress ----------------
// Git writes progress to stderr as "Phase:  42% (123/456), 1.2 MiB | 300 KiB/s", redrawn with \r.
static void onProgress(const std::string &chunk) {
  ui([chunk] {
    app.log += chunk;
    Strings lines;
    std::string cur;
    for (char c : chunk) { if (c == '\r' || c == '\n') { if (!trim(cur).empty()) lines.push_back(trim(cur)); cur.clear(); } else cur += c; }
    if (!trim(cur).empty()) lines.push_back(trim(cur));
    if (lines.empty()) return;
    std::string line = lines.back();
    static const std::regex chunkRe("(?:chunk|part|round) (\\d+)(?:/(\\d+))?", std::regex::icase);
    static const std::regex pct("^(?:remote:\\s*)?([A-Za-z ]+?):\\s+(\\d+)% \\(([\\d/]+)\\)(?:,\\s*([^|]+?)(?:\\s*\\|\\s*(.+?))?)?(?:,\\s*done\\.?)?$");
    std::smatch m;
    if (std::regex_search(line, m, chunkRe)) app.progressChunk = m[2].matched ? "chunk " + m[1].str() + "/" + m[2].str() : "round " + m[1].str();
    std::string prefix = app.progressLabel + (app.progressChunk.empty() ? "" : " (" + app.progressChunk + ")");
    if (std::regex_match(line, m, pct)) {
      app.progressPct = std::stof(m[2]) / 100.f;
      std::string size = m[4].matched && !startsWith(m[4].str(), "done") ? trim(m[4]) : "";
      app.setStatus(prefix + " — " + m[1].str() + " " + m[3].str() + (size.empty() ? "" : " · " + size) + (m[5].matched ? " · " + trim(m[5]) : ""));
    } else {
      app.setStatus(prefix + " — " + std::regex_replace(line, std::regex("^remote:\\s*"), ""));
    }
  });
}

void App::task(const std::string &label, std::function<void(const Progress &)> fn, bool refresh) {
  if (busy) { toast("Another operation is running"); return; }
  busy = true; progressLabel = label; progressChunk.clear(); progressPct = -1;
  setStatus(label + "…");
  log += "\n> " + label + "\n";
  bg([=] {
    std::string err;
    try { fn(onProgress); } catch (std::exception &e) { err = e.what(); }
    ui([=] {
      busy = false; progressPct = -1;
      if (err.empty()) setStatus(label + " — done");
      else { setStatus(label + " failed"); toast(label + " failed:\n" + err, true); log += err + "\n"; }
      // Refresh even on failure: earlier steps (e.g. adding a remote) may have succeeded.
      if (refresh && !repo.empty()) refreshRepo();
    });
  });
}

// ---------------- sync ----------------
void App::syncPrimary() {
  std::string r = currentRemote();
  if (repo.empty() || r.empty() || !status) { toast("This repository has no remotes. Add one under Remotes."); return; }
  if (status->upstream.empty()) { git::PushOpts o; o.setUpstream = true; doPush(r, o); return; }
  if (status->behind) { doPull(r); return; }
  if (status->ahead) { doPush(r); return; }
  std::string rp = repo;
  task("Fetching " + r, [=](const Progress &p) { git::fetch(rp, r, p); });
}

void App::doPull(const std::string &remote, int rebase) {
  if (!status) return;
  bool rb = rebase < 0 ? settings.value("pullRebase", false) : rebase == 1;
  std::string b = status->branch, rp = repo;
  if (chunkOn()) { int n = chunkSize(); task("Pulling " + remote + "/" + b + " in chunks", [=](const Progress &p) { git::pullChunked(rp, remote, b, n, p); }); }
  else task("Pulling " + remote + "/" + b, [=](const Progress &p) { git::pull(rp, remote, b, rb, p); });
}

void App::doPush(const std::string &remote, git::PushOpts o) {
  if (!status) return;
  std::string b = status->branch, rp = repo;
  if (chunkOn() && !o.force) { int n = chunkSize(); task("Pushing to " + remote + " in chunks", [=](const Progress &p) { git::pushChunked(rp, remote, b, n, p); }); return; }
  if (status->upstream.empty()) o.setUpstream = true;
  task("Pushing to " + remote, [=](const Progress &p) { git::push(rp, remote, b, o, p); });
}

// ---------------- diff parsing ----------------
ParsedDiff parseDiff(const std::string &text, const std::string &title, const std::string &kind) {
  ParsedDiff d; d.title = title; d.kind = kind;
  if (text.size() > 4'000'000) { d.tooLarge = true; return d; }
  Strings lines = split(text, '\n', true);
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  std::vector<std::string> header;
  size_t i = 0;
  while (i < lines.size() && !startsWith(lines[i], "@@")) header.push_back(lines[i++]);
  int oldN = 0, newN = 0;
  static const std::regex hre("-(\\d+)(?:,\\d+)? \\+(\\d+)");
  while (i < lines.size()) {
    const std::string &l = lines[i];
    if (startsWith(l, "diff --git")) {
      header.clear();
      std::string name = std::regex_replace(l, std::regex("^diff --git a/.* b/"), "");
      d.lines.push_back({ 'F', 0, 0, name, -1 });
      while (i < lines.size() && !startsWith(lines[i], "@@")) header.push_back(lines[i++]);
      continue;
    }
    if (startsWith(l, "@@")) {
      size_t end = i + 1;
      while (end < lines.size() && !startsWith(lines[end], "@@") && !startsWith(lines[end], "diff --git")) end++;
      DiffHunk h; h.header = l;
      for (auto &x : header) h.patch += x + "\n";
      for (size_t j = i; j < end; j++) h.patch += lines[j] + "\n";
      std::smatch m;
      if (std::regex_search(l, m, hre)) { oldN = std::stoi(m[1]); newN = std::stoi(m[2]); }
      d.hunks.push_back(h);
      d.lines.push_back({ '@', 0, 0, l, (int)d.hunks.size() - 1 });
      i++;
      continue;
    }
    char c = l.empty() ? ' ' : l[0];
    std::string t = l;
    if (!t.empty() && t.back() == '\r') t.pop_back();
    if (c == '+') d.lines.push_back({ '+', 0, newN++, t, (int)d.hunks.size() - 1 });
    else if (c == '-') d.lines.push_back({ '-', oldN++, 0, t, (int)d.hunks.size() - 1 });
    else if (c == '\\') d.lines.push_back({ '\\', 0, 0, t, (int)d.hunks.size() - 1 });
    else d.lines.push_back({ ' ', oldN++, newN++, t, (int)d.hunks.size() - 1 });
    i++;
  }
  return d;
}

// ---------------- boot / frame ----------------
void App::boot() {
  settings = vault::settings();
  saveSettings(json::object());
  float scale = ImGui::GetIO().FontDefault ? ImGui::GetIO().FontDefault->FontSize / 15.5f : 1.f;
  sidebarW = 280 * scale; paneW = 340 * scale;
  if (settings.contains("sidebarWidth") && settings["sidebarWidth"].is_number()) sidebarW = std::clamp(settings["sidebarWidth"].get<float>(), 200 * scale, 560 * scale);
  if (settings.contains("paneChanges") && settings["paneChanges"].is_number()) paneW = settings["paneChanges"].get<float>();
  loadRepos();
  std::string last = settings.value("lastRepo", "");
  Strings list = settings.value("repos", Strings{});
  if (!last.empty() && std::find(list.begin(), list.end(), last) != list.end()) openRepo(last);
}

void App::frame() {
  ImGuiIO &io = ImGui::GetIO();
  // Keyboard shortcuts.
  if (io.KeyCtrl && modals.empty()) {
    if (ImGui::IsKeyPressed(ImGuiKey_1)) tab = Tab::Changes;
    if (ImGui::IsKeyPressed(ImGuiKey_2)) tab = Tab::History;
    if (ImGui::IsKeyPressed(ImGuiKey_3)) tab = Tab::Branches;
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P)) syncPrimary();
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R)) refreshRepo();
    if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O)) addLocal();
    if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N)) createDialog();
  }

  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);

  drawSidebar();
  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  drawToolbar();
  drawMain();
  ImGui::EndGroup();
  drawStatusbar();
  ImGui::End();

  drawModals();
  drawToasts();
}
