#include "app.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

static float S(float v) { return v * ImGui::GetFontSize() / 15.5f; }  // scale with DPI via font size

static const ImVec4 LANE[] = { hexColor("#d97757"), hexColor("#5b8def"), hexColor("#4f9d69"), hexColor("#c58a2c"),
                               hexColor("#a26bd1"), hexColor("#d0566c"), hexColor("#3aa7a3"), hexColor("#8a8475") };
static ImVec4 laneColor(int i) { return LANE[i % 8]; }

// ---------------- commit graph layout ----------------
// lanes: commit hash each lane is waiting for. Output rows carry lanes before/after for drawing.
static void layoutGraph(const std::vector<git::Commit> &cs, std::vector<std::string> &lanes, std::vector<GraphRow> &rows) {
  for (auto &c : cs) {
    GraphRow r; r.before = lanes; r.parents = c.parents; r.merge = c.parents.size() > 1;
    auto it = std::find(lanes.begin(), lanes.end(), c.hash);
    int colI;
    if (it != lanes.end()) colI = (int)(it - lanes.begin());
    else { auto f = std::find(lanes.begin(), lanes.end(), ""); colI = f != lanes.end() ? (int)(f - lanes.begin()) : (int)lanes.size(); }
    for (auto &h : lanes) if (h == c.hash) h = "";
    if (colI >= (int)lanes.size()) lanes.resize(colI + 1);
    lanes[colI] = c.parents.empty() ? "" : c.parents[0];
    for (size_t p = 1; p < c.parents.size(); p++) {
      if (std::find(lanes.begin(), lanes.end(), c.parents[p]) != lanes.end()) continue;
      auto f = std::find(lanes.begin(), lanes.end(), "");
      if (f != lanes.end()) *f = c.parents[p]; else lanes.push_back(c.parents[p]);
    }
    while (!lanes.empty() && lanes.back().empty()) lanes.pop_back();
    r.col = colI; r.after = lanes;
    rows.push_back(r);
  }
}

static void drawGraphRow(const GraphRow &r, const std::string &hash, ImVec2 o, float h, float laneW) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  auto X = [&](int i) { return o.x + i * laneW + laneW / 2 + 2; };
  float mid = o.y + h / 2, th = S(2.f);
  auto curve = [&](float x1, float y1, float x2, float y2, ImU32 c) {
    if (x1 == x2) dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), c, th);
    else { float my = (y1 + y2) / 2; dl->AddBezierCubic(ImVec2(x1, y1), ImVec2(x1, my), ImVec2(x2, my), ImVec2(x2, y2), c, th, 12); }
  };
  for (int i = 0; i < (int)r.before.size(); i++) {
    const std::string &l = r.before[i];
    if (l.empty()) continue;
    if (l == hash) curve(X(i), o.y, X(r.col), mid, col(laneColor(i)));
    else { auto j = std::find(r.after.begin(), r.after.end(), l); if (j != r.after.end()) { int jj = (int)(j - r.after.begin()); curve(X(i), o.y, X(jj), o.y + h, col(laneColor(jj))); } }
  }
  for (auto &p : r.parents) {
    auto j = std::find(r.after.begin(), r.after.end(), p);
    if (j != r.after.end()) { int jj = (int)(j - r.after.begin()); curve(X(r.col), mid, X(jj), o.y + h, col(laneColor(jj))); }
  }
  ImVec2 c(X(r.col), mid);
  if (r.merge) { dl->AddCircleFilled(c, S(4), col(C.bg)); dl->AddCircle(c, S(4), col(laneColor(r.col)), 16, th); }
  else dl->AddCircleFilled(c, S(4.5f), col(laneColor(r.col)));
}

// ---------------- sidebar ----------------
void App::drawSidebar() {
  float statusH = S(28);
  float h = ImGui::GetContentRegionAvail().y - statusH;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, C.side);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(14)));
  ImGui::BeginChild("##sidebar", ImVec2(sidebarW, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();

  // Brand
  ImGui::PushFont(F.title);
  AppIcon(ImGui::GetTextLineHeight() * 1.15f);
  ImGui::TextUnformatted("KillerGit");
  ImGui::PopFont();
  ImGui::Spacing();

  // Primary "new" action, like Claude's "New chat".
  ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
  if (Button("  +   Add repository", false, false, ImVec2(-1, S(36)))) ImGui::OpenPopup("addmenu");
  ImGui::PopStyleVar();
  if (ImGui::BeginPopup("addmenu")) {
    if (ImGui::MenuItem("Clone repository…")) cloneDialog();
    if (ImGui::MenuItem("Add existing local repository…", "Ctrl+O")) addLocal();
    if (ImGui::MenuItem("Create new local repository…", "Ctrl+N")) createDialog();
    if (ImGui::MenuItem("Create repository on a host (and clone)…")) publishDialog(false, true);
    ImGui::Separator();
    if (ImGui::MenuItem("Import from GitHub Desktop…")) importGithubDesktop();
    ImGui::EndPopup();
  }
  ImGui::SetNextItemWidth(-S(64));
  ImGui::InputTextWithHint("##filter", "Search repositories", &filter);
  ImGui::SameLine();
  std::string sort = settings.value("repoSort", "az");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##sort", sort == "az" ? "A–Z" : "Recent", ImGuiComboFlags_NoArrowButton)) {
    if (ImGui::Selectable("A–Z", sort == "az")) saveSettings({ { "repoSort", "az" } });
    if (ImGui::Selectable("Recently updated", sort != "az")) saveSettings({ { "repoSort", "updated" } });
    ImGui::EndCombo();
  }
  ImGui::Spacing();

  // Tree: host → namespace → repos
  float footH = S(44);
  ImGui::BeginChild("##tree", ImVec2(0, ImGui::GetContentRegionAvail().y - footH));
  if (reposLoading && repos.empty()) { Spinner(S(8), S(2)); ImGui::SameLine(); Muted("Loading repositories…"); }
  std::map<std::string, std::map<std::string, std::vector<RepoEntry *>>> groups;
  for (auto &r : repos) {
    if (!filter.empty() && !icontains(r.name, filter) && !icontains(r.ns, filter)) continue;
    std::string host = r.missing ? "Missing" : r.host.empty() ? "Local only" : r.host;
    groups[host][r.ns.empty() && !r.host.empty() ? "(root)" : r.ns].push_back(&r);
  }
  if (groups.empty() && !reposLoading) Muted(repos.empty() ? "No repositories yet." : "No matches.");
  bool byDate = sort != "az";
  auto latest = [](const std::vector<RepoEntry *> &v) { long long m = 0; for (auto *r : v) m = std::max(m, r->lastActivity); return m; };
  std::vector<std::string> hosts;
  for (auto &[h, _] : groups) hosts.push_back(h);
  if (byDate) std::sort(hosts.begin(), hosts.end(), [&](auto &a, auto &b) {
    long long la = 0, lb = 0; for (auto &[_, v] : groups[a]) la = std::max(la, latest(v)); for (auto &[_, v] : groups[b]) lb = std::max(lb, latest(v)); return la > lb; });
  json collapsed = settings.value("collapsed", json::array());
  auto isCollapsed = [&](const std::string &k) { for (auto &c : collapsed) if (c == k) return true; return false; };
  auto toggle = [&](const std::string &k) {
    json n = json::array(); bool was = false;
    for (auto &c : collapsed) { if (c == k) was = true; else n.push_back(c); }
    if (!was) n.push_back(k);
    saveSettings({ { "collapsed", n } });
  };
  auto groupHeader = [&](const std::string &key, const std::string &label, size_t count, float indent) {
    ImGui::PushID(key.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool open = !isCollapsed(key);
    if (SelectableRow("##g", false, S(26))) toggle(key);
    ImGui::SetCursorScreenPos(ImVec2(p.x + indent, p.y + S(4)));
    ImGui::PushFont(F.small);
    ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
    ImGui::Text("%s  %s", open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", label.c_str());
    ImGui::SameLine(); ImGui::TextDisabled("%zu", count);
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(26)));
    ImGui::PopID();
    return open;
  };
  auto item = [&](RepoEntry &r, float indent) {
    ImGui::PushID(r.path.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (SelectableRow("##r", r.path == repo, S(30))) openRepo(r.path);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", r.path.c_str());
    if (ImGui::BeginPopupContextItem("ctx")) {
      if (ImGui::MenuItem(SHOW_IN_FM)) openPath(r.path);
      if (ImGui::MenuItem("Open in terminal")) openTerminal(r.path);
      if (ImGui::MenuItem("Open in editor")) openEditor(settings.value("editor", std::string("code")), r.path);
      if (ImGui::MenuItem("Copy path")) copyToClipboard(r.path);
      ImGui::Separator();
      if (ImGui::MenuItem("Remove from list")) {
        Strings list = settings.value("repos", Strings{}); std::string rp = r.path;
        list.erase(std::remove(list.begin(), list.end(), rp), list.end());
        saveSettings({ { "repos", list } });
        if (repo == rp) { repo.clear(); status.reset(); }
        ImGui::EndPopup(); ImGui::PopID(); loadRepos(); return false;
      }
      ImGui::EndPopup();
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float ty = p.y + (S(30) - ImGui::GetTextLineHeight()) / 2;
    ImVec4 tc = r.missing ? C.faint : r.pending ? C.muted : C.text;
    ImVec4 clip(p.x, p.y, p.x + w - S(70), p.y + S(30));
    dl->AddText(r.path == repo ? F.bold : F.body, ImGui::GetFontSize(), ImVec2(p.x + indent, ty), col(tc), r.name.c_str(), nullptr, 0, &clip);
    float rx = p.x + w - S(8);
    std::string when = ago(r.lastActivity);
    ImGui::PushFont(F.small);
    float ww = ImGui::CalcTextSize(when.c_str()).x;
    dl->AddText(ImVec2(rx - ww, p.y + (S(30) - ImGui::GetTextLineHeight()) / 2), col(C.faint), when.c_str());
    ImGui::PopFont();
    if (r.changed) dl->AddCircleFilled(ImVec2(rx - ww - S(10), p.y + S(15)), S(3.5f), col(C.accent));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(30)));
    ImGui::PopID();
    return true;
  };
  auto cmp = [&](RepoEntry *a, RepoEntry *b) { return byDate ? a->lastActivity > b->lastActivity : lower(a->name) < lower(b->name); };
  for (auto &host : hosts) {
    auto &nsMap = groups[host];
    size_t total = 0; for (auto &[_, v] : nsMap) total += v.size();
    if (!groupHeader(host, host, total, S(6))) continue;
    std::vector<std::string> nss; for (auto &[n, _] : nsMap) nss.push_back(n);
    if (byDate) std::sort(nss.begin(), nss.end(), [&](auto &a, auto &b) { return latest(nsMap[a]) > latest(nsMap[b]); });
    bool stop = false;
    for (auto &ns : nss) {
      auto &list = nsMap[ns];
      std::sort(list.begin(), list.end(), cmp);
      if (ns.empty()) { for (auto *r : list) if (!item(*r, S(14))) { stop = true; break; } }
      else if (groupHeader(host + "/" + ns, ns, list.size(), S(16))) { for (auto *r : list) if (!item(*r, S(28))) { stop = true; break; } }
      if (stop) break;
    }
    if (stop) break;
  }
  ImGui::EndChild();

  // Footer: accounts + settings
  ImGui::Separator();
  if (IconButton("Accounts & keys", "Accounts, SSH keys & tokens")) accountsPage();
  ImGui::SameLine();
  if (IconButton("\xE2\x9A\x99  Settings", "Settings")) settingsPage();
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // Resizer
  ImGui::SameLine(0, 0);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##sideResize", ImVec2(S(5), h));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemActive()) sidebarW = std::clamp(sidebarW + ImGui::GetIO().MouseDelta.x, S(200), S(560));
  if (ImGui::IsItemDeactivated()) saveSettings({ { "sidebarWidth", sidebarW } });
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { sidebarW = S(280); saveSettings({ { "sidebarWidth", sidebarW } }); }
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + h), col(ImGui::IsItemActive() || ImGui::IsItemHovered() ? C.accent : C.border));
}

// ---------------- toolbar ----------------
void App::drawToolbar() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
  ImGui::BeginChild("##toolbar", ImVec2(0, S(66)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  float iw = S(190);
  if (ToolbarItem("tb-repo", "Current repository", repo.empty() ? "—" : baseName(repo), iw)) {}
  ImGui::SameLine();
  if (ToolbarItem("tb-branch", "Current branch", status ? status->branch : "—", iw, "Switch or create a branch (Ctrl+B)") && !repo.empty()) {
    ImGui::OpenPopup("branchpop");
    std::string r = repo;
    bg([=] { try { auto bl = git::branches(r); ui([=] { if (repo == r) { branchList = bl; branchesLoaded = true; } }); } catch (...) {} });
  }
  if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B) && !repo.empty() && modals.empty()) ImGui::OpenPopup("branchpop");
  branchPopup();
  ImGui::SameLine();

  std::string remote = currentRemote();
  std::string label = "Fetch " + remote, sub = "Fetch";
  if (status && status->upstream.empty() && !remote.empty() && !status->branch.empty()) { label = "Publish branch"; sub = "to " + remote; }
  else if (status && status->behind) { label = "Pull " + remote + "  \xE2\x86\x93" + std::to_string(status->behind); sub = status->ahead ? "\xE2\x86\x91" + std::to_string(status->ahead) + " to push after" : "Behind remote"; }
  else if (status && status->ahead) { label = "Push " + remote + "  \xE2\x86\x91" + std::to_string(status->ahead); sub = "Ahead of remote"; }
  if (busy) { label = progressLabel; sub = progressPct >= 0 ? std::to_string((int)(progressPct * 100)) + "%" : "Working…"; }
  if (ToolbarItem("tb-sync", sub.c_str(), label, S(210), "Click to sync · right-click for more") && !busy) syncPrimary();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("syncmenu");
  ImGui::SameLine();
  auto needRemote = [&](auto fn) { if (repo.empty() || remote.empty()) toast("This repository has no remotes. Add one under Remotes."); else fn(); };
  if (ToolbarItem("tb-pull", "Pull", "\xE2\x86\x93 " + (status && status->behind ? std::to_string(status->behind) : std::string()), S(64), "Pull from the selected remote")) needRemote([&] { doPull(remote); });
  ImGui::SameLine();
  if (ToolbarItem("tb-push", "Push", "\xE2\x86\x91 " + (status && status->ahead ? std::to_string(status->ahead) : std::string()), S(64), "Push to the selected remote")) needRemote([&] { doPush(remote); });
  ImGui::SameLine();
  if (ToolbarItem("tb-more", "More", "\xE2\x96\xBE", S(54), "Multi-remote, force push, chunked")) ImGui::OpenPopup("syncmenu");
  if (ImGui::BeginPopup("syncmenu")) { syncMenuItems(); ImGui::EndPopup(); }
  ImGui::SameLine();
  if (ToolbarItem("tb-remote", "Remote", remote.empty() ? "none" : remote, S(130), "Remote used for fetch/pull/push")) ImGui::OpenPopup("remotemenu");
  if (ImGui::BeginPopup("remotemenu")) {
    ImGui::TextDisabled("Default remote for this repo");
    for (auto &r : remotes) {
      if (ImGui::MenuItem((r.name + "   " + r.fetch).c_str(), nullptr, r.name == remote)) {
        json rr = settings.value("repoRemote", json::object()); rr[repo] = r.name; saveSettings({ { "repoRemote", rr } });
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Manage remotes…")) tab = Tab::Remotes;
    ImGui::EndPopup();
  }
  ImGui::EndChild();
}

void App::syncMenuItems() {
  if (repo.empty() || !status) { ImGui::TextDisabled("Open a repository first"); return; }
  std::string r = currentRemote(), rp = repo, b = status->branch;
  ImGui::TextDisabled("Remote: %s", r.empty() ? "none" : r.c_str());
  if (ImGui::MenuItem("Fetch")) task("Fetching", [=](const Progress &p) { git::fetch(rp, r, p); });
  if (ImGui::MenuItem("Fetch all remotes")) task("Fetching all remotes", [=](const Progress &p) { git::fetchAll(rp, p); });
  if (ImGui::MenuItem("Pull (merge)")) doPull(r, 0);
  if (ImGui::MenuItem("Pull (rebase)")) doPull(r, 1);
  if (ImGui::MenuItem("Push")) doPush(r);
  if (ImGui::MenuItem("Force push (with lease)…"))
    confirm("Force push", "Overwrite " + r + "/" + b + " with your local branch?", "Force push", true, [=] { git::PushOpts o; o.force = true; doPush(r, o); });
  if (ImGui::MenuItem("Push with tags")) { git::PushOpts o; o.tags = true; doPush(r, o); }
  ImGui::Separator();
  ImGui::TextDisabled("Multiple remotes");
  Strings names; for (auto &x : remotes) names.push_back(x.name);
  if (ImGui::MenuItem("Push to all remotes"))
    task("Pushing to all remotes", [=](const Progress &p) { for (auto &n : names) git::push(rp, n, b, {}, p); });
  for (auto &n : names) if (n != r && ImGui::MenuItem(("Pull from " + n).c_str())) doPull(n);
  for (auto &n : names) if (n != r && ImGui::MenuItem(("Push to " + n).c_str())) doPush(n);
  ImGui::Separator();
  ImGui::TextDisabled("Slow connections");
  if (ImGui::MenuItem(("Chunked pull/push (" + std::to_string(chunkSize()) + " commits)").c_str(), nullptr, chunkOn())) saveSettings({ { "chunked", !chunkOn() } });
  if (ImGui::MenuItem("Fetch full history (unshallow)")) task("Unshallowing", [=](const Progress &p) { git::unshallow(rp, r, p); });
}

void App::branchPopup() {
  static std::string q;
  ImGui::SetNextWindowSizeConstraints(ImVec2(S(320), 0), ImVec2(S(420), S(520)));
  if (!ImGui::BeginPopup("branchpop")) return;
  if (ImGui::IsWindowAppearing()) { q.clear(); ImGui::SetKeyboardFocusHere(); }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##bq", "Filter branches", &q);
  if (ImGui::MenuItem("+  New branch…")) { ImGui::CloseCurrentPopup(); newBranch(""); }
  ImGui::Separator();
  ImGui::BeginChild("##bl", ImVec2(0, S(360)));
  std::vector<const git::Branch *> locals, remoteOnly;
  std::set<std::string> localNames;
  for (auto &b : branchList) if (b.type == "local") localNames.insert(b.name);
  for (auto &b : branchList) {
    if (!icontains(b.name, q)) continue;
    if (b.type == "local") locals.push_back(&b);
    else if (b.type == "remote" && !localNames.count(b.name.substr(b.name.find('/') + 1))) remoteOnly.push_back(&b);
  }
  std::sort(locals.begin(), locals.end(), [](auto *a, auto *b) { return a->date > b->date; });
  if (!branchesLoaded) { Spinner(S(7), S(2)); }
  ImGui::TextDisabled("Local");
  for (auto *b : locals) {
    std::string l = (b->current ? "\xE2\x9C\x93 " : "   ") + b->name + (b->track.empty() ? "" : "  " + b->track);
    if (ImGui::Selectable(l.c_str())) { git::Branch copy = *b; ImGui::CloseCurrentPopup(); switchBranch(copy, false); }
  }
  if (!remoteOnly.empty()) {
    ImGui::TextDisabled("Remote");
    int n = 0;
    for (auto *b : remoteOnly) { if (n++ > 50) break; if (ImGui::Selectable(("   " + b->name).c_str())) { git::Branch copy = *b; ImGui::CloseCurrentPopup(); switchBranch(copy, true); } }
  }
  ImGui::EndChild();
  ImGui::EndPopup();
}

void App::switchBranch(const git::Branch &b, bool remote) {
  std::string rp = repo;
  if (remote) {
    std::string local = b.name.substr(b.name.find('/') + 1), name = b.name;
    task("Checking out " + name, [=](const Progress &) { git::checkoutRemote(rp, name, local); });
    return;
  }
  std::string name = b.name;
  auto go = [=] { task("Switching to " + name, [=](const Progress &) { git::checkout(rp, name); }); };
  if (status && !status->files.empty()) {
    std::string from = status->branch;
    modal("Uncommitted changes", [=] {
      ImGui::TextWrapped("You have uncommitted changes. What should happen to them?");
      ImGui::Spacing();
      if (Button("Cancel")) return false;
      ImGui::SameLine();
      if (Button("Bring them along")) { go(); return false; }
      ImGui::SameLine();
      if (Button("Stash them", true)) {
        task("Stashing and switching to " + name, [=](const Progress &) { git::stash(rp, "Auto-stash on branch switch from " + from); git::checkout(rp, name); });
        return false;
      }
      return true;
    });
  } else go();
}

void App::newBranch(const std::string &from) {
  std::string rp = repo;
  prompt("Create branch", from.empty() ? "Based on " + (status ? status->branch : std::string("HEAD")) : "Based on " + from.substr(0, 10), "",
    [=](std::string n) {
      for (auto &c : n) if (c == ' ') c = '-';
      task("Creating " + n, [=](const Progress &) { git::createBranch(rp, n, from); });
    });
}

// ---------------- main area ----------------
void App::drawMain() {
  float statusH = S(28);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), 0));
  ImGui::BeginChild("##main", ImVec2(0, ImGui::GetContentRegionAvail().y - statusH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  if (repo.empty()) { viewWelcome(); ImGui::EndChild(); return; }

  // Segmented tabs
  ImVec2 tp = ImGui::GetCursorScreenPos();
  const char *names[] = { "Home", "Changes", "History", "Branches", "Stashes", "Remotes", "Issues", "Console" };
  float tw = 0; for (auto *n : names) tw += ImGui::CalcTextSize(n).x + S(28) + S(4);
  ImGui::GetWindowDrawList()->AddRectFilled(tp, ImVec2(tp.x + tw + S(8), tp.y + ImGui::GetFrameHeight() + S(8)), col(C.side), 999);
  ImGui::SetCursorScreenPos(ImVec2(tp.x + S(4), tp.y + S(4)));
  for (int i = 0; i < 8; i++) {
    if (i) ImGui::SameLine(0, S(4));
    std::string l = names[i];
    if (i == (int)Tab::Changes && status && !status->files.empty()) l += "  " + std::to_string(status->files.size());
    if (Pill((l + "##tab" + std::to_string(i)).c_str(), (int)tab == i)) tab = (Tab)i;
  }
  ImGui::SetCursorScreenPos(ImVec2(tp.x, tp.y + ImGui::GetFrameHeight() + S(16)));

  if (repoLoading || (!status && repoGen > 0 && repoLoading)) {
    Spinner(S(10), S(2.5f)); ImGui::SameLine(); Muted("Opening " + baseName(repo) + "…");
    ImGui::EndChild(); return;
  }
  if (!status) { Empty("Repository unavailable", repo); ImGui::EndChild(); return; }

  if (!conflict.empty()) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(C.accent.x, C.accent.y, C.accent.z, 0.12f));
    ImGui::BeginChild("##banner", ImVec2(0, S(44)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("A %s is in progress. Resolve conflicts, stage the files, then continue.", conflict.c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(170) + ImGui::GetCursorPosX());
    std::string rp = repo, cf = conflict;
    if (Button("Continue", true)) task("Continuing", [=](const Progress &) { git::continueOp(rp, cf); });
    ImGui::SameLine();
    if (Button("Abort", false, true)) task("Aborting", [=](const Progress &) { git::abort(rp); });
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }

  switch (tab) {
    case Tab::Home: viewHome(); break;
    case Tab::Changes: viewChanges(); break;
    case Tab::History: viewHistory(); break;
    case Tab::Branches: viewBranches(); break;
    case Tab::Stashes: viewStashes(); break;
    case Tab::Remotes: viewRemotes(); break;
    case Tab::Issues: viewIssues(); break;
    case Tab::Console: viewConsole(); break;
  }
  ImGui::EndChild();
}

void App::viewWelcome() {
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float w = std::min(S(460), avail.x - S(40));
  ImGui::SetCursorPos(ImVec2((avail.x - w) / 2, avail.y * 0.22f));
  ImGui::BeginGroup();
  ImGui::PushFont(F.headingLarge);
  AppIcon(ImGui::GetTextLineHeight() * 1.1f);
  ImGui::TextUnformatted("Welcome to KillerGit");
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
  Muted("Clone a repository, add one from your disk, or start something new.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing(); ImGui::Spacing();
  if (Button("Clone repository", true, false, ImVec2(w, S(40)))) cloneDialog();
  if (Button("Add local repository", false, false, ImVec2(w, S(40)))) addLocal();
  if (Button("Create new repository", false, false, ImVec2(w, S(40)))) createDialog();
  if (Button("Import from GitHub Desktop", false, false, ImVec2(w, S(40)))) importGithubDesktop();
  ImGui::Spacing();
  if (IconButton("Set up accounts & SSH keys \xE2\x86\x92", nullptr)) accountsPage();
  ImGui::EndGroup();
}

// Resizable two-pane split; returns left width.
static float splitter(const char *id, float &w, float minW, float maxW, const json &settings, const std::string &key) {
  (void)settings; (void)key;
  ImGui::SameLine(0, 0);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetContentRegionAvail().y;
  ImGui::InvisibleButton(id, ImVec2(S(9), h));
  bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (act) w = std::clamp(w + ImGui::GetIO().MouseDelta.x, minW, maxW);
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + S(4), p.y), ImVec2(p.x + S(4), p.y + h), col(act || hov ? C.accent : C.border));
  ImGui::SameLine(0, 0);
  return w;
}

// ---------------- changes ----------------
struct Suggestion { std::string summary, desc; };
static Suggestion suggestMessage(const std::vector<git::FileStatus> &files) {
  std::vector<const git::FileStatus *> staged, set;
  for (auto &f : files) if (f.staged) staged.push_back(&f);
  if (!staged.empty()) set = staged; else for (auto &f : files) set.push_back(&f);
  if (set.empty()) return {};
  auto kind = [&](const git::FileStatus *f) -> std::string {
    char c = !staged.empty() ? f->index : (f->work != ' ' ? f->work : f->index);
    return c == '?' || c == 'A' ? "Add" : c == 'D' ? "Delete" : c == 'R' ? "Rename" : "Update";
  };
  auto base = [](const std::string &p) { auto i = p.find_last_of('/'); return i == std::string::npos ? p : p.substr(i + 1); };
  auto joinNames = [](const Strings &n) { if (n.size() == 1) return n[0]; return join(Strings(n.begin(), n.end() - 1), ", ") + " and " + n.back(); };
  std::vector<std::string> verbs; std::map<std::string, std::vector<const git::FileStatus *>> groups;
  for (auto *f : set) { auto k = kind(f); if (!groups.count(k)) verbs.push_back(k); groups[k].push_back(f); }
  Suggestion s;
  if (verbs.size() == 1 && set.size() <= 3) { Strings n; for (auto *f : set) n.push_back(base(f->path)); s.summary = verbs[0] + " " + joinNames(n); }
  else if (verbs.size() == 1) {
    Strings common = split(set[0]->path, '/'); common.pop_back();
    for (auto *f : set) { Strings d = split(f->path, '/'); d.pop_back(); size_t i = 0; while (i < common.size() && i < d.size() && common[i] == d[i]) i++; common.resize(i); }
    s.summary = verbs[0] + " " + std::to_string(set.size()) + " files" + (common.empty() ? "" : " in " + join(common, "/"));
  } else {
    Strings parts;
    for (size_t i = 0; i < verbs.size(); i++) {
      auto &g = groups[verbs[i]];
      parts.push_back((i ? lower(verbs[i]) : verbs[i]) + " " + (g.size() == 1 ? base(g[0]->path) : std::to_string(g.size()) + " files"));
    }
    s.summary = join(parts, ", ");
  }
  if (set.size() >= 2) {
    std::map<std::string, std::string> past = { { "Add", "Added" }, { "Delete", "Deleted" }, { "Rename", "Renamed" }, { "Update", "Updated" } };
    Strings lines;
    for (auto &v : verbs) for (auto *f : groups[v]) lines.push_back("- " + past[v] + " " + (f->orig.empty() ? "" : f->orig + " \xE2\x86\x92 ") + f->path);
    s.desc = join(lines, "\n");
  }
  return s;
}

static char codeFor(const git::FileStatus &f) {
  if (f.index == '?') return '?';
  if (f.index == 'U' || f.work == 'U') return 'U';
  return f.work != ' ' ? f.work : f.index;
}

void App::loadFileDiff(const git::FileStatus &f) {
  std::string key = f.path + "|" + f.index + f.work;
  if (diffFor == key) return;
  diffFor = key; diffLoading = true; fileDiffs.clear();
  int gen = ++diffGen; std::string rp = repo; git::FileStatus file = f;
  bg([=] {
    std::vector<ParsedDiff> out; std::string err;
    try {
      bool untracked = file.index == '?';
      if (file.staged) out.push_back(parseDiff(git::diff(rp, file.path, true, false), "Staged \xC2\xB7 " + file.path, "staged"));
      if (file.unstaged || untracked) out.push_back(parseDiff(git::diff(rp, file.path, false, untracked), (untracked ? "New file \xC2\xB7 " : "Unstaged \xC2\xB7 ") + file.path, untracked ? "untracked" : "unstaged"));
    } catch (std::exception &e) { err = e.what(); }
    ui([=] {
      if (gen != diffGen) return;
      diffLoading = false; fileDiffs = out;
      if (!err.empty()) toast(err, true);
    });
  });
}

void App::discardFiles(const std::vector<git::FileStatus> &files) {
  std::string rp = repo;
  confirm("Discard changes", "Discard changes to " + std::to_string(files.size()) + " file(s)? This cannot be undone.", "Discard", true, [=] {
    Strings untracked, tracked, added;
    for (auto &f : files) { if (f.index == '?') untracked.push_back(f.path); else if (f.index == 'A') added.push_back(f.path); else tracked.push_back(f.path); }
    task("Discarding", [=](const Progress &) {
      Strings u = untracked;
      if (!added.empty()) { git::unstage(rp, added); u.insert(u.end(), added.begin(), added.end()); }
      git::discard(rp, tracked, u);
    });
  });
}

void App::viewChanges() {
  auto &files = status->files;
  float h = ImGui::GetContentRegionAvail().y - S(6);
  paneW = std::clamp(paneW, S(260), std::max(S(280), ImGui::GetContentRegionAvail().x - S(320)));
  std::string rp = repo;

  ImGui::BeginChild("##changesL", ImVec2(paneW, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  // header
  bool allStaged = !files.empty(), anyStaged = false;
  for (auto &f : files) { if (!(f.staged && !f.unstaged)) allStaged = false; if (f.staged) anyStaged = true; }
  bool cb = allStaged;
  if (anyStaged && !allStaged) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
  if (ImGui::Checkbox("##all", &cb)) task("Staging", [=](const Progress &) { if (cb) git::stageAll(rp); else git::unstage(rp, { "." }); });
  if (anyStaged && !allStaged) ImGui::PopItemFlag();
  ImGui::SameLine();
  ImGui::Text("%zu changed file%s", files.size(), files.size() == 1 ? "" : "s");
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(30));
  if (IconButton("\xE2\x8B\xAF", "More")) ImGui::OpenPopup("chmore");
  if (ImGui::BeginPopup("chmore")) {
    if (ImGui::MenuItem("Discard all changes…")) discardFiles(files);
    if (ImGui::MenuItem("Stash all changes")) task("Stashing", [=](const Progress &) { git::stash(rp); });
    if (ImGui::MenuItem("Undo last commit")) task("Undoing commit", [=](const Progress &) { git::undoCommit(rp); });
    if (ImGui::MenuItem("Refresh", "Ctrl+Shift+R")) refreshRepo();
    ImGui::EndPopup();
  }

  float commitH = S(290);
  ImGui::BeginChild("##files", ImVec2(0, ImGui::GetContentRegionAvail().y - commitH));
  if (selFile.empty() && !files.empty()) selFile = files[0].path;
  ImGuiListClipper clip; clip.Begin((int)files.size(), S(30));
  while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
    auto &f = files[i];
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (SelectableRow("##row", selFile == f.path, S(30))) selFile = f.path;
    if (ImGui::BeginPopupContextItem("fctx")) {
      std::string path = f.path; git::FileStatus copy = f;
      if (ImGui::MenuItem("Discard changes…")) discardFiles({ copy });
      if (ImGui::MenuItem("Ignore file")) task("Ignoring", [=](const Progress &) { appendFile(rp + KG_SEP ".gitignore", "\n" + path); });
      auto dot = path.find_last_of('.');
      if (dot != std::string::npos && dot > path.find_last_of('/') + 1) {
        std::string ext = path.substr(dot + 1);
        if (ImGui::MenuItem(("Ignore all *." + ext + " files").c_str())) task("Ignoring", [=](const Progress &) { appendFile(rp + KG_SEP ".gitignore", "\n*." + ext); });
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Copy path")) copyToClipboard(path);
      if (ImGui::MenuItem(SHOW_IN_FM)) showInExplorer(rp + KG_SEP + toBackslash(path));
      if (ImGui::MenuItem("Open with default program")) openPath(rp + KG_SEP + toBackslash(path));
      ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(6), p.y + (S(30) - ImGui::GetFrameHeight()) / 2));
    bool on = f.staged && !f.unstaged;
    bool mixed = f.staged && f.unstaged;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(3), S(3)));
    if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::Checkbox("##s", &on)) { std::string path = f.path; task("Staging", [=](const Progress &) { if (on) git::stage(rp, { path }); else git::unstage(rp, { path }); }); }
    if (mixed) ImGui::PopItemFlag();
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y + (S(30) - ImGui::GetTextLineHeight()) / 2));
    Badge(codeFor(f));
    ImGui::SameLine();
    std::string name = f.path; auto sl = name.find_last_of('/');
    ImGui::TextUnformatted(sl == std::string::npos ? name.c_str() : name.substr(sl + 1).c_str());
    if (sl != std::string::npos) { ImGui::SameLine(); ImGui::PushFont(F.small); ImGui::TextDisabled("%s", name.substr(0, sl).c_str()); ImGui::PopFont(); }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(30)));
    ImGui::PopID();
  }
  ImGui::EndChild();

  // Commit box
  ImGui::PushStyleColor(ImGuiCol_ChildBg, C.card);
  ImGui::PushStyleColor(ImGuiCol_Border, C.border);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
  ImGui::BeginChild("##commit", ImVec2(0, commitH - S(6)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  static Suggestion sug; static std::string sugFor;
  std::string fk; for (auto &f : files) fk += f.path + f.index + f.work + "|";
  if (fk != sugFor) { sug = suggestMessage(files); sugFor = fk; }
  ImGui::SetNextItemWidth(-S(40));
  std::string hint = sug.summary.empty() ? "Summary (required)" : sug.summary + "  (Tab to accept)";
  bool tabAccept = draftSummary.empty() && !sug.summary.empty();
  ImGui::InputTextWithHint("##sum", hint.c_str(), &draftSummary, tabAccept ? ImGuiInputTextFlags_CallbackCompletion : 0,
    [](ImGuiInputTextCallbackData *d) { auto *s = (Suggestion *)d->UserData; d->InsertChars(0, s->summary.c_str()); return 0; }, &sug);
  ImGui::SameLine();
  if (IconButton("\xE2\x9C\xB3", "Autofill summary and description from changes") && !files.empty()) { draftSummary = sug.summary; draftDesc = sug.desc; }
  ImGui::InputTextMultiline("##desc", &draftDesc, ImVec2(-1, S(80)));
  if (draftDesc.empty() && !ImGui::IsItemActive()) {
    ImVec2 mn = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + S(10), mn.y + S(7)), col(C.faint), "Description");
  }
  if (ImGui::Checkbox("Amend last commit", &amend) && amend && draftSummary.empty()) {
    bg([=] { try { auto l = git::log(rp, 0, 1, false); if (!l.empty()) { std::string s = l[0].subject; ui([=] { if (draftSummary.empty()) draftSummary = s; }); } } catch (...) {} });
  }
  bool chunk = settings.value("chunkCommit", false);
  if (ImGui::Checkbox("Commit in chunks of", &chunk)) saveSettings({ { "chunkCommit", chunk } });
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split staged files into several smaller commits");
  ImGui::SameLine();
  int chunkN = settings.value("chunkCommitSize", 100);
  ImGui::SetNextItemWidth(S(64));
  if (ImGui::InputInt("##cn", &chunkN, 0)) saveSettings({ { "chunkCommitSize", std::max(1, chunkN) } });
  ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("files");
  int stagedCount = 0; for (auto &f : files) if (f.staged) stagedCount++;
  std::string btn = "Commit " + (stagedCount ? std::to_string(stagedCount) : std::string("all")) + " to " + status->branch;
  ImGui::BeginDisabled(files.empty() && !amend);
  if (Button(btn.c_str(), true, false, ImVec2(-1, S(36)))) {
    if (trim(draftSummary).empty() && !amend) toast("Enter a commit summary");
    else ensureIdentity([=] {
      std::string msg = trim(draftSummary) + (trim(draftDesc).empty() ? "" : "\n\n" + trim(draftDesc));
      bool am = amend, ch = chunk; int n = chunkN, sc = stagedCount;
      task("Committing", [=](const Progress &p) {
        if (!sc) git::stageAll(rp);
        if (ch && !am) git::commitChunked(rp, msg, n, p);
        else git::commit(rp, msg, am);
        ui([=] { draftSummary.clear(); draftDesc.clear(); amend = false; });
      });
    });
  }
  ImGui::EndDisabled();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  ImGui::EndChild();

  splitter("##chsplit", paneW, S(260), std::max(S(280), ImGui::GetWindowWidth() - S(340)), settings, "changes");
  if (ImGui::IsItemDeactivated()) saveSettings({ { "paneChanges", paneW } });

  ImGui::BeginChild("##changesR", ImVec2(0, h));
  const git::FileStatus *sel = nullptr;
  for (auto &f : files) if (f.path == selFile) sel = &f;
  if (!sel && !files.empty()) sel = &files[0];
  if (!sel) {
    Empty("No local changes", status->ahead ? "You have " + std::to_string(status->ahead) + " commit(s) to push." : "Working tree is clean.");
    ImGui::Spacing();
    float bw = S(300); ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) / 2);
    if (Button("Open in editor", false, false, ImVec2(bw / 2 - S(4), 0))) openEditor(settings.value("editor", std::string("code")), repo);
    ImGui::SameLine();
    if (Button(SHOW_IN_FM, false, false, ImVec2(bw / 2 - S(4), 0))) openPath(repo);
  } else {
    loadFileDiff(*sel);
    if (diffLoading) { Spinner(S(8), S(2)); ImGui::SameLine(); Muted("Loading diff…"); }
    for (size_t i = 0; i < fileDiffs.size(); i++) drawDiff(fileDiffs[i], ("d" + std::to_string(i)).c_str());
  }
  ImGui::EndChild();
}

void App::drawDiff(const ParsedDiff &d, const char *id) {
  ImGui::PushID(id);
  ImGui::PushFont(F.bold); ImGui::TextUnformatted(d.title.c_str()); ImGui::PopFont();
  if (d.tooLarge) { Muted("Diff too large to display."); ImGui::PopID(); return; }
  if (d.lines.empty()) { Muted("No textual changes (binary or mode change)."); ImGui::PopID(); return; }
  std::string rp = repo;
  ImGui::PushFont(F.mono);
  float lh = ImGui::GetTextLineHeight() + S(3);
  float numW = ImGui::CalcTextSize("00000").x;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  float w = ImGui::GetContentRegionAvail().x;
  ImGuiListClipper clip; clip.Begin((int)d.lines.size(), lh);
  while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
    const DiffLine &l = d.lines[i];
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (l.kind == '@' || l.kind == 'F') {
      dl->AddRectFilled(p, ImVec2(p.x + w, p.y + lh), col(C.hunkBg), 6);
      ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), p.y + S(1)));
      ImGui::PushStyleColor(ImGuiCol_Text, l.kind == 'F' ? C.text : C.muted);
      ImGui::TextUnformatted(l.text.c_str());
      ImGui::PopStyleColor();
      if (l.kind == '@' && l.hunk >= 0) {
        std::string patch = d.hunks[l.hunk].patch;
        ImGui::PushFont(F.small);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), 0));
        float bx = p.x + w - S(8);
        auto hb = [&](const char *label, bool danger) {
          float bw = ImGui::CalcTextSize(label).x + S(16); bx -= bw + S(6);
          ImGui::SetCursorScreenPos(ImVec2(bx, p.y + S(1)));
          ImGui::PushID(i);
          bool r = Button(label, false, danger, ImVec2(bw, lh - S(2)));
          ImGui::PopID();
          return r;
        };
        if (d.kind == "unstaged") {
          if (hb("Discard", true)) confirm("Discard hunk", "Discard this hunk? This cannot be undone.", "Discard", true,
            [=] { task("Discarding hunk", [=](const Progress &) { git::applyPatch(rp, patch, false, true); }); });
          if (hb("Stage hunk", false)) task("Staging hunk", [=](const Progress &) { git::applyPatch(rp, patch, true, false); });
        } else if (d.kind == "staged") {
          if (hb("Unstage hunk", false)) task("Unstaging hunk", [=](const Progress &) { git::applyPatch(rp, patch, true, true); });
        }
        ImGui::PopStyleVar();
        ImGui::PopFont();
      }
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lh));
      continue;
    }
    if (l.kind == '+') dl->AddRectFilled(p, ImVec2(p.x + w, p.y + lh), col(C.addBg));
    if (l.kind == '-') dl->AddRectFilled(p, ImVec2(p.x + w, p.y + lh), col(C.delBg));
    int no = l.kind == '+' ? l.newNo : l.kind == '-' ? l.oldNo : l.kind == ' ' ? l.newNo : 0;
    if (no) { char b[16]; snprintf(b, sizeof b, "%5d", no); dl->AddText(ImVec2(p.x, p.y + S(1)), col(C.faint), b); }
    ImVec4 tc = l.kind == '+' ? C.add : l.kind == '-' ? C.del : l.kind == '\\' ? C.faint : C.text;
    dl->AddText(ImVec2(p.x + numW + S(12), p.y + S(1)), col(tc), l.text.c_str());
    ImGui::Dummy(ImVec2(numW + S(12) + ImGui::CalcTextSize(l.text.c_str()).x, lh));
  }
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::PopID();
}

// ---------------- history ----------------
void App::loadHistory(bool more) {
  if (historyLoading) return;
  historyLoading = true;
  std::string rp = repo; bool all = settings.value("historyAll", true);
  int skip = more ? (int)commits.size() : 0;
  bg([=] {
    std::vector<git::Commit> cs; std::string err;
    Strings up; std::string head;
    try { cs = git::log(rp, skip, 400, all); if (!more) { up = git::unpushed(rp); head = trim(git::run(rp, { "rev-parse", "HEAD" }, "", nullptr, true, "")); } } catch (std::exception &e) { err = e.what(); }
    ui([=] {
      historyLoading = false;
      if (repo != rp) return;
      if (!err.empty()) { toast(err, true); return; }
      if (!more) { commits.clear(); rows.clear(); lanes.clear(); unpushedSet = std::set<std::string>(up.begin(), up.end()); headHash = head; }
      layoutGraph(cs, lanes, rows);
      commits.insert(commits.end(), cs.begin(), cs.end());
      historyMore = cs.size() == 400; historyLoaded = true;
    });
  });
}

void App::showCommit(const std::string &hash) {
  selCommit = hash; commitDetail.reset(); commitDiff.clear(); commitFile.clear();
  int gen = ++commitGen; std::string rp = repo;
  bg([=] {
    try {
      auto d = git::show(rp, hash);
      ui([=] { if (gen != commitGen) return; commitDetail = d; if (!d.files.empty()) loadCommitFile(d.files[0].second); });
    } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); }
  });
}

void App::loadCommitFile(const std::string &file) {
  commitFile = file; commitDiff.clear();
  int gen = commitGen; std::string rp = repo, h = selCommit;
  bg([=] {
    try {
      auto d = parseDiff(git::showFile(rp, h, file), file, "commit");
      ui([=] { if (gen == commitGen && commitFile == file) commitDiff = { d }; });
    } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); }
  });
}

void App::commitContextMenu(const git::Commit &c) {
  std::string rp = repo, h = c.hash, subj = c.subject;
  if (ImGui::MenuItem("Copy SHA")) copyToClipboard(h);
  if (ImGui::MenuItem("Copy subject")) copyToClipboard(subj);
  ImGui::Separator();
  if (ImGui::MenuItem("Create branch from commit…")) newBranch(h);
  if (ImGui::MenuItem("Create tag…")) prompt("Create tag", "Tag name", "", [=](std::string n) { task("Tagging", [=](const Progress &) { git::tag(rp, n, h); }); });
  if (ImGui::MenuItem("Checkout (detached)")) task("Checking out", [=](const Progress &) { git::checkout(rp, h); });
  ImGui::Separator();
  if (ImGui::MenuItem("Cherry-pick onto current branch")) task("Cherry-picking", [=](const Progress &) { git::cherryPick(rp, h); });
  if (h == headHash && unpushedSet.count(h)) {
    ImGui::Separator();
    if (ImGui::MenuItem("Undo commit (keep changes)")) task("Undoing commit", [=](const Progress &) { git::undoCommit(rp); });
  }
  if (ImGui::MenuItem("Revert this commit")) task("Reverting", [=](const Progress &) { git::revert(rp, h); });
  if (ImGui::MenuItem("Reset current branch here (soft)")) task("Resetting", [=](const Progress &) { git::resetTo(rp, h, "soft"); });
  if (ImGui::MenuItem("Reset current branch here (mixed)")) task("Resetting", [=](const Progress &) { git::resetTo(rp, h, "mixed"); });
  if (ImGui::MenuItem("Reset current branch here (hard)…"))
    confirm("Hard reset", "All uncommitted changes will be lost.", "Reset", true, [=] { task("Resetting", [=](const Progress &) { git::resetTo(rp, h, "hard"); }); });
}

void App::viewHistory() {
  if (!historyLoaded && !historyLoading) loadHistory(false);
  float h = ImGui::GetContentRegionAvail().y - S(6);
  static float histW = -1;
  if (histW < 0) histW = settings.value("paneHistory", ImGui::GetContentRegionAvail().x * 0.58f);
  histW = std::clamp(histW, S(300), std::max(S(320), ImGui::GetContentRegionAvail().x - S(300)));

  ImGui::BeginChild("##histL", ImVec2(histW, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  ImGui::SetNextItemWidth(-S(130));
  ImGui::InputTextWithHint("##hf", "Filter by message, author or SHA", &historyFilter);
  ImGui::SameLine();
  bool all = settings.value("historyAll", true);
  if (ImGui::Checkbox("All branches", &all)) { saveSettings({ { "historyAll", all } }); historyLoaded = false; }
  ImGui::BeginChild("##hlist");
  if (historyLoading && commits.empty()) { Spinner(S(8), S(2)); ImGui::SameLine(); Muted("Wrangling the commit graph…"); }
  std::vector<int> idx;
  for (int i = 0; i < (int)commits.size(); i++) {
    auto &c = commits[i];
    if (historyFilter.empty() || icontains(c.subject, historyFilter) || icontains(c.author, historyFilter) || startsWith(c.hash, lower(historyFilter))) idx.push_back(i);
  }
  int maxLanes = 1;
  for (auto &r : rows) maxLanes = std::max({ maxLanes, (int)r.before.size(), (int)r.after.size(), r.col + 1 });
  float laneW = S(14), graphW = historyFilter.empty() ? std::min(maxLanes * laneW + S(8), S(260)) : S(8);
  float rowH = S(30);
  ImGuiListClipper clip; clip.Begin((int)idx.size(), rowH);
  while (clip.Step()) for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
    int i = idx[k]; auto &c = commits[i];
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (SelectableRow("##c", selCommit == c.hash, rowH)) showCommit(c.hash);
    if (ImGui::BeginPopupContextItem("cctx")) { commitContextMenu(c); ImGui::EndPopup(); }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p, ImVec2(p.x + graphW, p.y + rowH), true);
    if (historyFilter.empty() && i < (int)rows.size()) drawGraphRow(rows[i], c.hash, p, rowH, laneW);
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p.x + graphW + S(4), p.y + (rowH - ImGui::GetTextLineHeight()) / 2));
    for (auto &r : c.refs) {
      bool head = startsWith(r, "HEAD -> ") || r == "HEAD";
      std::string name = r; if (startsWith(name, "HEAD -> ")) name = name.substr(8);
      bool tag = startsWith(name, "tag: "); if (tag) name = name.substr(5);
      ImVec4 rc = head ? C.accent : tag ? hexColor("#c58a2c") : name.find('/') != std::string::npos ? hexColor("#5b8def") : hexColor("#4f9d69");
      RefChip(name, rc); ImGui::SameLine(0, S(4));
    }
    std::string meta = c.author + " \xC2\xB7 " + ago(c.time);
    ImGui::PushFont(F.small);
    float mw = ImGui::CalcTextSize(meta.c_str()).x;
    ImGui::PopFont();
    ImVec2 tp = ImGui::GetCursorScreenPos();
    ImVec4 clipR(tp.x, p.y, p.x + w - mw - S(16), p.y + rowH);
    dl->AddText(F.body, ImGui::GetFontSize(), tp, col(C.text), c.subject.c_str(), nullptr, 0, &clipR);
    ImGui::PushFont(F.small);
    dl->AddText(ImVec2(p.x + w - mw - S(8), p.y + (rowH - ImGui::GetTextLineHeight()) / 2), col(C.muted), meta.c_str());
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::PopID();
  }
  if (historyMore && historyFilter.empty()) {
    ImGui::Spacing();
    if (historyLoading) Spinner(S(7), S(2));
    else if (Button("Load more", false, false, ImVec2(-1, 0))) loadHistory(true);
  }
  ImGui::EndChild();
  ImGui::EndChild();

  splitter("##hsplit", histW, S(300), std::max(S(320), ImGui::GetWindowWidth() - S(300)), settings, "history");
  if (ImGui::IsItemDeactivated()) saveSettings({ { "paneHistory", histW } });

  ImGui::BeginChild("##histR", ImVec2(0, h));
  if (selCommit.empty()) Empty("Select a commit", "Its message and changes appear here.");
  else if (!commitDetail) { Spinner(S(8), S(2)); ImGui::SameLine(); Muted("Loading commit…"); }
  else {
    auto &d = *commitDetail;
    std::string body = trim(d.body), subject = body, rest;
    auto nl = body.find('\n'); if (nl != std::string::npos) { subject = body.substr(0, nl); rest = trim(body.substr(nl + 1)); }
    ImGui::PushFont(F.heading); ImGui::TextWrapped("%s", subject.c_str()); ImGui::PopFont();
    if (!rest.empty()) Muted(rest);
    ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
    ImGui::Text("%s \xC2\xB7 %s \xC2\xB7", d.author.c_str(), formatTime(d.time).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C.accent);
    ImGui::TextUnformatted(d.hash.substr(0, 10).c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip("Copy full SHA"); }
    if (ImGui::IsItemClicked()) { copyToClipboard(d.hash); toast("SHA copied"); }
    ImGui::Spacing();
    for (size_t i = 0; i < d.files.size(); i++) {
      ImGui::PushID((int)i);
      ImVec2 p = ImGui::GetCursorScreenPos();
      if (SelectableRow("##f", commitFile == d.files[i].second, S(28))) loadCommitFile(d.files[i].second);
      ImGui::SetCursorScreenPos(ImVec2(p.x + S(6), p.y + (S(28) - ImGui::GetTextLineHeight()) / 2));
      Badge(d.files[i].first); ImGui::SameLine(); ImGui::TextUnformatted(d.files[i].second.c_str());
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(28)));
      ImGui::PopID();
    }
    ImGui::Separator();
    for (size_t i = 0; i < commitDiff.size(); i++) drawDiff(commitDiff[i], ("cd" + std::to_string(i)).c_str());
  }
  ImGui::EndChild();
}

// ---------------- branches ----------------
void App::viewBranches() {
  std::string rp = repo;
  if (!branchesLoaded) {
    branchesLoaded = true;
    bg([=] { try { auto bl = git::branches(rp); ui([=] { if (repo == rp) branchList = bl; }); } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); } });
  }
  ImGui::BeginChild("##branches");
  if (Button("New branch", true)) newBranch("");
  ImGui::Spacing();
  std::vector<git::Branch> local, remote, tags;
  for (auto &b : branchList) (b.type == "local" ? local : b.type == "remote" ? remote : tags).push_back(b);
  auto byDate = [](auto &a, auto &b) { return a.date > b.date; };
  std::sort(local.begin(), local.end(), byDate); std::sort(remote.begin(), remote.end(), byDate); std::sort(tags.begin(), tags.end(), byDate);
  std::string cur = status ? status->branch : "";
  auto table = [&](const char *title, const char *id, std::vector<git::Branch> &list, int kind) {
    Heading(title);
    if (list.empty()) { Muted("None."); ImGui::Spacing(); return; }
    if (ImGui::BeginTable(id, kind == 0 ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
      if (kind == 0) ImGui::TableSetupColumn("Upstream", ImGuiTableColumnFlags_WidthStretch, 0.6f);
      ImGui::TableSetupColumn("Updated", ImGuiTableColumnFlags_WidthFixed, S(90));
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, kind == 1 ? S(230) : kind == 0 ? S(130) : S(80));
      ImGui::PushStyleColor(ImGuiCol_Text, C.muted); ImGui::TableHeadersRow(); ImGui::PopStyleColor();
      for (size_t i = 0; i < list.size(); i++) {
        auto b = list[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow(0, S(34));
        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
        if (b.current) { ImGui::PushFont(F.bold); ImGui::Text("\xE2\x9C\x93 %s", b.name.c_str()); ImGui::PopFont(); } else ImGui::TextUnformatted(b.name.c_str());
        if (kind == 0) { ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s %s", b.upstream.empty() ? "—" : b.upstream.c_str(), b.track.c_str()); }
        ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", ago(b.date).c_str());
        ImGui::TableNextColumn();
        if (kind == 0) {
          if (!b.current) { if (Button("Checkout")) switchBranch(b, false); ImGui::SameLine(); }
          if (Button("\xE2\x8B\xAF")) ImGui::OpenPopup("bm");
          if (ImGui::BeginPopup("bm")) {
            std::string n = b.name;
            if (!b.current && ImGui::MenuItem(("Merge into " + cur).c_str())) task("Merging", [=](const Progress &) { git::merge(rp, n); });
            if (!b.current && ImGui::MenuItem(("Squash-merge into " + cur).c_str())) task("Squash merging", [=](const Progress &) { git::merge(rp, n, true); });
            if (!b.current && ImGui::MenuItem(("Rebase " + cur + " onto this").c_str())) task("Rebasing", [=](const Progress &) { git::rebase(rp, n); });
            if (ImGui::MenuItem("Rename…")) prompt("Rename branch", "New name", n, [=](std::string nn) { task("Renaming", [=](const Progress &) { git::renameBranch(rp, n, nn); }); });
            if (ImGui::MenuItem("Create branch from here…")) newBranch(n);
            if (!b.current && ImGui::MenuItem("Delete…"))
              confirm("Delete branch", "Delete " + n + "?", "Delete", true, [=] { task("Deleting", [=](const Progress &) { git::deleteBranch(rp, n, true); }); });
            ImGui::EndPopup();
          }
        } else if (kind == 1) {
          if (Button("Checkout")) switchBranch(b, true);
          ImGui::SameLine();
          std::string n = b.name;
          if (Button("Merge")) task("Merging", [=](const Progress &) { git::merge(rp, n); });
          ImGui::SameLine();
          if (Button("Delete", false, true)) {
            auto slash = n.find('/'); std::string r = n.substr(0, slash), br = n.substr(slash + 1);
            confirm("Delete remote branch", "Delete " + n + " on the server?", "Delete", true,
              [=] { task("Deleting remote branch", [=](const Progress &) { git::deleteRemoteBranch(rp, r, br); }); });
          }
        } else {
          std::string n = b.name;
          if (Button("Delete", false, true)) task("Deleting tag", [=](const Progress &) { git::deleteTag(rp, n); });
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::Spacing(); ImGui::Spacing();
  };
  table("Local branches", "##lt", local, 0);
  table("Remote branches", "##rt", remote, 1);
  if (!tags.empty()) table("Tags", "##tt", tags, 2);
  ImGui::EndChild();
}

// ---------------- stashes ----------------
void App::viewStashes() {
  std::string rp = repo;
  if (!stashesLoaded) {
    stashesLoaded = true;
    bg([=] { try { auto s = git::stashList(rp); ui([=] { if (repo == rp) stashes = s; }); } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); } });
  }
  ImGui::BeginChild("##stashes");
  if (Button("Stash current changes", true))
    modal("Stash changes", [=, msg = std::make_shared<std::string>()] {
      ImGui::TextUnformatted("Message (optional)");
      ImGui::SetNextItemWidth(-1); ImGui::InputText("##m", msg.get());
      if (Button("Cancel")) return false;
      ImGui::SameLine();
      if (Button("Stash", true)) { std::string m = *msg; task("Stashing", [=](const Progress &) { git::stash(rp, m); }); return false; }
      return true;
    });
  ImGui::Spacing();
  if (stashes.empty()) Muted("No stashes.");
  for (size_t i = 0; i < stashes.size(); i++) {
    auto s = stashes[i];
    ImGui::PushID((int)i);
    BeginCard("##card");
    ImGui::PushFont(F.bold); ImGui::TextUnformatted(s.msg.c_str()); ImGui::PopFont();
    Muted(s.ref + " \xC2\xB7 " + ago(s.time));
    if (Button("Restore", true)) task("Applying stash", [=](const Progress &) { git::stashPop(rp, s.ref); });
    ImGui::SameLine();
    if (Button("View")) {
      bg([=] {
        try {
          auto d = parseDiff(git::run(rp, { "stash", "show", "-p", "--include-untracked", s.ref }, "", nullptr, true), "", "commit");
          ui([=] { modal(s.msg, [=] { ImGui::BeginChild("##sd", ImVec2(S(820), S(520))); drawDiff(d, "sd"); ImGui::EndChild(); return !Button("Close", true); }, S(860)); });
        } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); }
      });
    }
    ImGui::SameLine();
    if (Button("Discard", false, true)) confirm("Drop stash", "Delete this stash?", "Delete", true, [=] { task("Dropping", [=](const Progress &) { git::stashDrop(rp, s.ref); }); });
    EndCard();
    ImGui::PopID();
  }
  ImGui::EndChild();
}

// ---------------- remotes ----------------
void App::viewRemotes() {
  std::string rp = repo;
  ImGui::BeginChild("##remotes");
  if (Button("Create repository on a host…", true)) publishDialog(true, false);
  ImGui::SameLine();
  if (Button("Add remote URL…")) addRemoteDialog();
  ImGui::SameLine();
  if (Button("Fetch all")) task("Fetching all", [=](const Progress &p) { git::fetchAll(rp, p); });
  Muted("A repository can have any number of remotes. Pick the default in the toolbar; push and pull to others from the sync menu.");
  ImGui::Spacing();
  if (remotes.empty()) Muted("No remotes configured.");
  std::string def = currentRemote();
  for (size_t i = 0; i < remotes.size(); i++) {
    auto r = remotes[i];
    ImGui::PushID((int)i);
    BeginCard("##rc");
    ImGui::PushFont(F.bold); ImGui::TextUnformatted(r.name.c_str()); ImGui::PopFont();
    if (r.name == def) { ImGui::SameLine(); RefChip("default", C.accent); }
    ImGui::PushFont(F.mono); ImGui::TextDisabled("fetch: %s", r.fetch.c_str()); if (r.push != r.fetch) ImGui::TextDisabled("push:  %s", r.push.c_str()); ImGui::PopFont();
    {
      json accts = vault::accounts();
      std::string cur = git::accountOverride(rp, r.fetch), curLabel = "Automatic (match host)";
      auto lbl = [](const json &a) { std::string l = a.value("label", ""); if (l.empty()) l = a.value("username", ""); if (l.empty()) l = a.value("provider", ""); return l + " â " + a.value("host", ""); };
      if (cur == "none") curLabel = "None (no account)";
      else if (!cur.empty()) for (auto &a : accts) if (a.value("id", "") == cur) curLabel = lbl(a);
      Muted("Account"); ImGui::SameLine(); ImGui::SetNextItemWidth(S(280));
      if (ImGui::BeginCombo("##acct", curLabel.c_str())) {
        if (ImGui::Selectable("Automatic (match host)", cur.empty())) git::setAccountOverride(rp, r.fetch, "");
        if (ImGui::Selectable("None (no account)", cur == "none")) git::setAccountOverride(rp, r.fetch, "none");
        for (auto &a : accts) { std::string id = a.value("id", ""); if (ImGui::Selectable(lbl(a).c_str(), id == cur)) git::setAccountOverride(rp, r.fetch, id); }
        ImGui::EndCombo();
      }
    }
    if (Button("Make default")) { json rr = settings.value("repoRemote", json::object()); rr[repo] = r.name; saveSettings({ { "repoRemote", rr } }); }
    ImGui::SameLine(); if (Button("Fetch")) task("Fetching " + r.name, [=](const Progress &p) { git::fetch(rp, r.name, p); });
    ImGui::SameLine(); if (Button("Pull")) doPull(r.name);
    ImGui::SameLine(); if (Button("Push")) doPush(r.name);
    ImGui::SameLine(); if (Button("Edit URL")) prompt("Edit remote URL", r.name, r.fetch, [=](std::string u) { task("Updating", [=](const Progress &) { git::setRemoteUrl(rp, r.name, u); }); });
    ImGui::SameLine(); if (Button("Copy URL")) copyToClipboard(r.fetch);
    ImGui::SameLine();
    if (Button("Remove", false, true)) confirm("Remove remote", "Remove " + r.name + "?", "Remove", true, [=] { task("Removing", [=](const Progress &) { git::removeRemote(rp, r.name); }); });
    EndCard();
    ImGui::PopID();
  }
  ImGui::EndChild();
}

// ---------------- issues ----------------
void App::loadIssues() {
  if (remotes.empty()) return;
  issueRemote = std::clamp(issueRemote, 0, (int)remotes.size() - 1);
  std::string url = remotes[issueRemote].fetch;
  const char *states[] = { "open", "closed", "all" };
  std::string st = states[issueState];
  issuesLoading = true; issuesError.clear(); issuesLoaded = true;
  bg([=] {
    std::vector<providers::Issue> is; std::string err;
    try { is = providers::issues(url, st); } catch (std::exception &e) { err = e.what(); }
    ui([=] { issuesLoading = false; issues = is; issuesError = err; });
  });
}

void App::openIssue(const providers::Issue &i) {
  selIssue = i; issueComments.clear(); issueReply.clear();
  std::string url = remotes[issueRemote].fetch; long long id = i.id;
  bg([=] {
    try { auto cs = providers::comments(url, id); ui([=] { if (selIssue && selIssue->id == id) issueComments = cs; }); }
    catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); }
  });
}

void App::viewIssues() {
  if (remotes.empty()) { Empty("No remotes", "Issues come from the hosting service of a remote."); return; }
  if (!issuesLoaded) {
    for (int i = 0; i < (int)remotes.size(); i++) if (remotes[i].name == currentRemote()) issueRemote = i;
    loadIssues();
  }
  float h = ImGui::GetContentRegionAvail().y - S(6);
  static float issW = -1;
  if (issW < 0) issW = settings.value("paneIssues", S(420));
  ImGui::BeginChild("##issL", ImVec2(issW, h));
  ImGui::SetNextItemWidth(issW * 0.45f);
  if (ImGui::BeginCombo("##ir", remotes[std::clamp(issueRemote, 0, (int)remotes.size() - 1)].name.c_str())) {
    for (int i = 0; i < (int)remotes.size(); i++) if (ImGui::Selectable((remotes[i].name + "  " + remotes[i].fetch).c_str(), i == issueRemote)) { issueRemote = i; loadIssues(); }
    ImGui::EndCombo();
  }
  ImGui::SameLine(); ImGui::SetNextItemWidth(S(90));
  const char *stNames[] = { "Open", "Closed", "All" };
  if (ImGui::Combo("##is", &issueState, stNames, 3)) loadIssues();
  ImGui::SameLine();
  std::string url = remotes[issueRemote].fetch;
  if (Button("New", true)) {
    auto t = std::make_shared<std::string>(), b = std::make_shared<std::string>();
    modal("New issue", [=] {
      ImGui::TextUnformatted("Title"); ImGui::SetNextItemWidth(-1); ImGui::InputText("##t", t.get());
      ImGui::TextUnformatted("Description"); ImGui::InputTextMultiline("##b", b.get(), ImVec2(-1, S(180)));
      if (Button("Cancel")) return false;
      ImGui::SameLine();
      if (Button("Create", true)) {
        std::string tt = *t, bb = *b;
        task("Creating issue", [=](const Progress &) { providers::createIssue(url, tt, bb); ui([=] { loadIssues(); }); }, false);
        return false;
      }
      return true;
    }, S(560));
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##iq", "Filter issues", &issueFilter);
  ImGui::BeginChild("##ilist");
  if (issuesLoading) { Spinner(S(8), S(2)); ImGui::SameLine(); Muted("Summoning issues…"); }
  else if (!issuesError.empty()) Muted(issuesError);
  else if (issues.empty()) Muted("No issues.");
  for (size_t k = 0; k < issues.size(); k++) {
    auto &i = issues[k];
    if (!issueFilter.empty() && !icontains(i.title, issueFilter) && std::to_string(i.id) != issueFilter) continue;
    ImGui::PushID((int)k);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float rh = S(52) + (i.labels.empty() ? 0 : S(20));
    if (SelectableRow("##i", selIssue && selIssue->id == i.id, rh)) openIssue(i);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), p.y + S(6)));
    ImGui::PushStyleColor(ImGuiCol_Text, i.state == "open" ? hexColor("#4f9d69") : hexColor("#a26bd1"));
    ImGui::TextUnformatted(i.state == "open" ? "\xE2\x97\x8F" : "\xE2\x9C\x93");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + issW - S(50));
    ImGui::TextUnformatted(i.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PushFont(F.small);
    ImGui::TextDisabled("#%lld by %s \xC2\xB7 %s \xC2\xB7 %d comments", i.id, i.author.c_str(), ago(i.created).c_str(), i.comments);
    ImGui::PopFont();
    for (auto &l : i.labels) { RefChip(l, C.muted); ImGui::SameLine(0, S(4)); }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(p.y + rh, ImGui::GetItemRectMax().y + S(6))));
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::EndChild();

  splitter("##isplit", issW, S(300), std::max(S(320), ImGui::GetWindowWidth() - S(300)), settings, "issues");
  if (ImGui::IsItemDeactivated()) saveSettings({ { "paneIssues", issW } });

  ImGui::BeginChild("##issR", ImVec2(0, h));
  if (!selIssue) Empty("Select an issue", "");
  else {
    auto i = *selIssue;
    ImGui::PushFont(F.heading); ImGui::TextWrapped("%s", i.title.c_str()); ImGui::PopFont();
    ImGui::SameLine(); ImGui::TextDisabled("#%lld", i.id);
    ImGui::TextDisabled("%s \xC2\xB7 opened by %s \xC2\xB7", i.state.c_str(), i.author.c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C.accent); ImGui::TextUnformatted("Open in browser"); ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) openUrl(i.web);
    ImGui::Spacing();
    BeginCard("##body");
    if (i.body.empty()) Muted("No description"); else ImGui::TextWrapped("%s", i.body.c_str());
    EndCard();
    for (size_t k = 0; k < issueComments.size(); k++) {
      ImGui::PushID((int)k);
      BeginCard("##cm");
      ImGui::PushFont(F.small); ImGui::TextDisabled("%s \xC2\xB7 %s", issueComments[k].author.c_str(), ago(issueComments[k].created).c_str()); ImGui::PopFont();
      ImGui::TextWrapped("%s", issueComments[k].body.c_str());
      EndCard();
      ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::InputTextMultiline("##reply", &issueReply, ImVec2(-1, S(100)));
    long long id = i.id; bool open = i.state == "open"; std::string body = issueReply;
    if (Button("Comment", true) && !trim(body).empty())
      task("Commenting", [=](const Progress &) { providers::comment(url, id, body); ui([=] { if (selIssue && selIssue->id == id) openIssue(*selIssue); }); }, false);
    ImGui::SameLine();
    if (Button(open ? "Close issue" : "Reopen issue"))
      task(open ? "Closing issue" : "Reopening issue", [=](const Progress &) {
        providers::setIssueState(url, id, !open);
        ui([=] { if (selIssue && selIssue->id == id) selIssue->state = open ? "closed" : "open"; loadIssues(); });
      }, false);
  }
  ImGui::EndChild();
}

// ---------------- status bar + toasts ----------------
void App::drawStatusbar() {
  ImGuiViewport *vp = ImGui::GetMainViewport();
  float h = S(28);
  ImGui::SetCursorScreenPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(p, ImVec2(p.x + vp->WorkSize.x, p.y + h), col(C.side));
  dl->AddLine(p, ImVec2(p.x + vp->WorkSize.x, p.y), col(C.border));
  ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y + (h - ImGui::GetTextLineHeight() * 0.85f) / 2 - S(1)));
  ImGui::PushFont(F.small);
  if (busy) { ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y + S(7))); Spinner(S(7), S(2)); ImGui::SameLine(); }
  ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
  ImGui::TextUnformatted(statusText.c_str());
  ImGui::PopStyleColor();
  float right = p.x + vp->WorkSize.x - S(12);
  if (busy) {
    float bw = S(180), bx = right - bw - S(80);
    ImVec2 a(bx, p.y + h / 2 - S(2)), b(bx + bw, p.y + h / 2 + S(2));
    dl->AddRectFilled(a, b, col(C.border), 4);
    if (progressPct >= 0) dl->AddRectFilled(a, ImVec2(a.x + bw * progressPct, b.y), col(C.accent), 4);
    else { float t = fmodf((float)ImGui::GetTime(), 1.4f) / 1.4f; float s = a.x + (bw - bw * 0.3f) * t; dl->AddRectFilled(ImVec2(s, a.y), ImVec2(s + bw * 0.3f, b.y), col(C.accent), 4); }
  }
  ImGui::SetCursorScreenPos(ImVec2(right - S(60), p.y + S(5)));
  if (ImGui::SmallButton("Output")) {
    modal("Output", [this] {
      ImGui::PushFont(F.mono);
      ImGui::InputTextMultiline("##log", &log, ImVec2(S(760), S(420)), ImGuiInputTextFlags_ReadOnly);
      ImGui::PopFont();
      if (Button("Clear")) log.clear();
      ImGui::SameLine();
      return !Button("Close", true);
    }, S(800));
  }
  ImGui::PopFont();
}

void App::drawToasts() {
  double t = now();
  toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](auto &x) { return x.until < t; }), toasts.end());
  ImGuiViewport *vp = ImGui::GetMainViewport();
  float y = vp->WorkPos.y + vp->WorkSize.y - S(44);
  for (int i = (int)toasts.size() - 1; i >= 0; i--) {
    auto &x = toasts[i];
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(16), y), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(240), 0), ImVec2(S(440), S(300)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, x.error ? ImVec4(C.danger.x * 0.25f + C.card.x * 0.75f, C.danger.y * 0.25f + C.card.y * 0.75f, C.danger.z * 0.25f + C.card.z * 0.75f, 1) : C.card);
    ImGui::PushStyleColor(ImGuiCol_Border, C.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    std::string id = "##toast" + std::to_string(i);
    ImGui::Begin(id.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushTextWrapPos(S(410));
    ImGui::TextUnformatted(x.text.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::IsWindowHovered()) x.until = std::max(x.until, t + 2);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) x.until = 0;
    y -= ImGui::GetWindowHeight() + S(8);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
  }
}
