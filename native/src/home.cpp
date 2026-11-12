// Home tab: a repository landing page in the style of a git hosting site —
// last commit, file browser, project information, language stats and the README.
#include "app.h"
#include <algorithm>
#include <map>

static float S(float v) { return v * ImGui::GetFontSize() / 15.5f; }

namespace {

struct Lang { const char *name; const char *color; };

// Extension -> language (only source-like files count, like linguist).
const Lang *langFor(const std::string &path) {
  static const std::map<std::string, Lang> M = {
    { "c", { "C", "#555555" } }, { "h", { "C++", "#f34b7d" } }, { "cpp", { "C++", "#f34b7d" } }, { "cc", { "C++", "#f34b7d" } },
    { "cxx", { "C++", "#f34b7d" } }, { "hpp", { "C++", "#f34b7d" } }, { "hh", { "C++", "#f34b7d" } }, { "inl", { "C++", "#f34b7d" } },
    { "cs", { "C#", "#178600" } }, { "java", { "Java", "#b07219" } }, { "kt", { "Kotlin", "#a97bff" } },
    { "js", { "JavaScript", "#f1e05a" } }, { "mjs", { "JavaScript", "#f1e05a" } }, { "jsx", { "JavaScript", "#f1e05a" } },
    { "ts", { "TypeScript", "#3178c6" } }, { "tsx", { "TypeScript", "#3178c6" } }, { "py", { "Python", "#3572a5" } },
    { "rs", { "Rust", "#dea584" } }, { "go", { "Go", "#00add8" } }, { "rb", { "Ruby", "#701516" } }, { "php", { "PHP", "#4f5d95" } },
    { "swift", { "Swift", "#f05138" } }, { "m", { "Objective-C", "#438eff" } }, { "mm", { "Objective-C++", "#6866fb" } },
    { "lua", { "Lua", "#000080" } }, { "sh", { "Shell", "#89e051" } }, { "bash", { "Shell", "#89e051" } },
    { "ps1", { "PowerShell", "#012456" } }, { "bat", { "Batchfile", "#c1f12e" } }, { "cmd", { "Batchfile", "#c1f12e" } },
    { "html", { "HTML", "#e34c26" } }, { "htm", { "HTML", "#e34c26" } }, { "css", { "CSS", "#663399" } }, { "scss", { "SCSS", "#c6538c" } },
    { "vue", { "Vue", "#41b883" } }, { "svelte", { "Svelte", "#ff3e00" } }, { "dart", { "Dart", "#00b4ab" } },
    { "hlsl", { "HLSL", "#aace60" } }, { "usf", { "HLSL", "#aace60" } }, { "ush", { "HLSL", "#aace60" } }, { "glsl", { "GLSL", "#5686a5" } },
    { "vert", { "GLSL", "#5686a5" } }, { "frag", { "GLSL", "#5686a5" } }, { "cmake", { "CMake", "#da3434" } },
    { "zig", { "Zig", "#ec915c" } }, { "nim", { "Nim", "#ffc200" } }, { "hs", { "Haskell", "#5e5086" } }, { "ex", { "Elixir", "#6e4a7e" } },
    { "scala", { "Scala", "#c22d40" } }, { "r", { "R", "#198ce7" } }, { "sql", { "SQL", "#e38c00" } }, { "gd", { "GDScript", "#355570" } },
  };
  std::string b = lower(baseName(path));
  if (b == "cmakelists.txt") return &M.at("cmake");
  auto d = b.rfind('.');
  if (d == std::string::npos) return nullptr;
  auto it = M.find(b.substr(d + 1));
  return it == M.end() ? nullptr : &it->second;
}

ImVec4 langColor(const std::string &name) {
  static std::map<std::string, ImVec4> cache;
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  ImVec4 c = hexColor("#8a8475");
  for (const char *e : { "c", "cpp", "cs", "java", "kt", "js", "ts", "py", "rs", "go", "rb", "php", "swift", "m", "mm", "lua", "sh", "ps1",
                          "bat", "html", "css", "scss", "vue", "svelte", "dart", "hlsl", "glsl", "cmake", "zig", "nim", "hs", "ex", "scala", "r", "sql", "gd" })
    if (auto *l = langFor(std::string("x.") + e); l && name == l->name) { c = hexColor(l->color); break; }
  return cache[name] = c;
}

Strings splitNul(const std::string &s) {
  Strings out; size_t a = 0;
  for (size_t i = 0; i < s.size(); i++) if (s[i] == '\0') { if (i > a) out.push_back(s.substr(a, i - a)); a = i + 1; }
  if (a < s.size()) out.push_back(s.substr(a));
  return out;
}

std::string humanSize(long long bytes) {
  const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
  double v = (double)bytes; int i = 0;
  while (v >= 1024 && i < 4) { v /= 1024; i++; }
  char b[32]; snprintf(b, sizeof b, i ? "%.1f %s" : "%.0f %s", v, u[i]);
  return b;
}

void Avatar(const std::string &name, float size) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  unsigned h = 0; for (char c : name) h = h * 31 + (unsigned char)c;
  static const char *pal[] = { "#d97757", "#5b8def", "#4f9d69", "#c58a2c", "#a26bd1", "#d0566c", "#3aa7a3" };
  dl->AddCircleFilled(ImVec2(p.x + size / 2, p.y + size / 2), size / 2, col(hexColor(pal[h % 7])));
  std::string ini = name.empty() ? "?" : std::string(1, (char)toupper((unsigned char)name[0]));
  ImVec2 ts = ImGui::CalcTextSize(ini.c_str());
  dl->AddText(ImVec2(p.x + (size - ts.x) / 2, p.y + (size - ts.y) / 2), IM_COL32_WHITE, ini.c_str());
  ImGui::Dummy(ImVec2(size, size));
}

void StatRow(const char *icon, const std::string &value, const char *label) {
  ImGui::PushStyleColor(ImGuiCol_Text, C.muted); ImGui::TextUnformatted(icon); ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::PushFont(F.bold); ImGui::TextUnformatted(value.c_str()); ImGui::PopFont();
  ImGui::SameLine(0, S(4));
  ImGui::TextUnformatted(label);
}

}  // namespace

// ---------------- loading ----------------
// dirOnly: keep repo-wide stats and only reload the listing + README (folder navigation).
void App::loadHome(bool dirOnly) {
  std::string rp = repo, path = homePath;
  int gen = ++homeGen;
  homeLoading = true; homeDetails = true;
  bool full = !dirOnly || !home;
  HomeData prev = home ? *home : HomeData{};
  bg([=] {
    HomeData d;
    if (!full) { d = prev; d.entries.clear(); d.readme.clear(); d.readmeName.clear(); }
    try {
      auto R = [&](Strings a) { return git::run(rp, a, "", nullptr, true); };
      if (full) {
        d.head = trim(R({ "rev-parse", "--abbrev-ref", "HEAD" }));
        auto last = split(trim(R({ "log", "-1", "--format=%H%x1f%an%x1f%at%x1f%s" })), '\x1f', true);
        if (last.size() >= 4) { d.lastHash = last[0]; d.lastAuthor = last[1]; d.lastTime = atoll(last[2].c_str()); d.lastSubject = last[3]; }
        auto roots = split(trim(R({ "log", "--format=%at", "--max-parents=0", "HEAD" })), '\n');
        if (!roots.empty()) d.created = atoll(roots.back().c_str());
        d.commits = atoi(trim(R({ "rev-list", "--count", "HEAD" })).c_str());
        d.branches = (int)split(trim(R({ "for-each-ref", "--format=x", "refs/heads" })), '\n').size();
        d.tags = (int)split(trim(R({ "for-each-ref", "--format=x", "refs/tags" })), '\n').size();
        for (auto &l : split(R({ "count-objects", "-v" }), '\n')) {
          auto kv = split(l, ':');
          if (kv.size() == 2 && (trim(kv[0]) == "size" || trim(kv[0]) == "size-pack")) d.storage += atoll(trim(kv[1]).c_str()) * 1024;
        }
        // languages: blob sizes across HEAD
        std::map<std::string, long long> langs;
        for (auto &e : splitNul(R({ "ls-tree", "-r", "-l", "-z", "HEAD" }))) {
          auto tab = e.find('\t'); if (tab == std::string::npos) continue;
          auto f = split(e.substr(0, tab), ' ');
          if (f.size() < 4 || f[1] != "blob") continue;
          if (auto *l = langFor(e.substr(tab + 1))) langs[l->name] += atoll(f[3].c_str());
        }
        for (auto &kv : langs) d.langs.push_back(kv);
        std::sort(d.langs.begin(), d.langs.end(), [](auto &a, auto &b) { return a.second > b.second; });
      }
      // directory listing
      Strings args = { "ls-tree", "-z", "HEAD" };
      if (!path.empty()) { args.push_back("--"); args.push_back(path + "/"); }
      for (auto &e : splitNul(R(args))) {
        auto tab = e.find('\t'); if (tab == std::string::npos) continue;
        auto f = split(e.substr(0, tab), ' ');
        HomeEntry he; he.name = baseName(e.substr(tab + 1)); he.dir = f.size() > 1 && f[1] == "tree";
        d.entries.push_back(he);
      }
      std::stable_sort(d.entries.begin(), d.entries.end(), [](auto &a, auto &b) {
        if (a.dir != b.dir) return a.dir;
        return lower(a.name) < lower(b.name);
      });
      for (auto &e : d.entries)
        if (!e.dir && startsWith(lower(e.name), "readme")) { d.readmeName = e.name; break; }
      if (!d.readmeName.empty()) d.readme = R({ "show", "HEAD:" + (path.empty() ? "" : path + "/") + d.readmeName });
    } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); }
    ui([=] { if (repo == rp && gen == homeGen) { home = d; homeLoading = false; } });

    // second pass: last commit touching each entry (capped so huge folders stay responsive)
    std::vector<std::pair<long long, std::string>> info;
    size_t n = std::min<size_t>(d.entries.size(), 300);
    if (!n) ui([=] { if (gen == homeGen) homeDetails = false; });
    for (size_t i = 0; i < n; i++) {
      if (homeGen.load() != gen) return;
      std::string full = (path.empty() ? "" : path + "/") + d.entries[i].name;
      std::string out;
      try { out = trim(git::run(rp, { "log", "-1", "--format=%at%x1f%s", "HEAD", "--", full }, "", nullptr, true)); } catch (...) {}
      auto p = split(out, '\x1f', true);
      info.push_back(p.size() >= 2 ? std::make_pair(atoll(p[0].c_str()), p[1]) : std::make_pair(0LL, std::string()));
      if (i % 25 == 24 || i + 1 == n)
        ui([=] {
          if (repo != rp || gen != homeGen || !home) return;
          if (info.size() == n) homeDetails = false;
          for (size_t k = 0; k < info.size() && k < home->entries.size(); k++) { home->entries[k].time = info[k].first; home->entries[k].subject = info[k].second; }
        });
    }
  });
}

// ---------------- view ----------------
void App::viewHome() {
  if (!home && !homeLoading) loadHome();
  if (!home) { Spinner(S(10), S(2.5f)); ImGui::SameLine(); Muted("Loading\xE2\x80\xA6"); return; }
  const HomeData &d = *home;
  std::string rp = repo;

  ImGui::BeginChild("##home");
  float avail = ImGui::GetContentRegionAvail().x;
  bool side = avail > S(720);
  float sideW = side ? S(260) : 0;
  float mainW = side ? avail - sideW - S(20) : avail;

  // title
  ImGui::PushFont(F.headingLarge); AppIcon(ImGui::GetTextLineHeight()); ImGui::TextUnformatted(baseName(repo).c_str()); ImGui::PopFont();
  ImGui::Spacing();

  ImGui::BeginGroup();
  ImGui::BeginChild("##homemain", ImVec2(mainW, 0), ImGuiChildFlags_AutoResizeY);
  // branch + breadcrumb
  RefChip(d.head.empty() ? "HEAD" : d.head, C.accent);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  std::optional<std::string> navTo;
  std::string crumb;
  if (ImGui::SmallButton(baseName(repo).c_str())) navTo = "";
  if (!homePath.empty()) {
    for (auto &part : split(homePath, '/')) {
      crumb += (crumb.empty() ? "" : "/") + part;
      ImGui::SameLine(0, S(2)); Muted("/"); ImGui::SameLine(0, S(2));
      if (ImGui::SmallButton((part + "##c" + crumb).c_str())) navTo = crumb;
    }
  }
  if (homeLoading) { ImGui::SameLine(0, S(8)); Spinner(S(7), S(2)); }
  ImGui::Spacing();

  // last commit card
  if (!d.lastHash.empty()) {
    BeginCard("##lastcommit");
    Avatar(d.lastAuthor, S(30));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(F.bold); ImGui::TextUnformatted(d.lastSubject.c_str()); ImGui::PopFont();
    Muted(d.lastAuthor + " authored " + ago(d.lastTime));
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(150) + ImGui::GetCursorPosX());
    ImGui::PushFont(F.mono);
    if (ImGui::Button(d.lastHash.substr(0, 8).c_str())) { copyToClipboard(d.lastHash); toast("Copied commit hash"); }
    ImGui::PopFont();
    ImGui::SameLine();
    if (Button("History")) tab = Tab::History;
    EndCard();
  }

  // file browser
  ImGuiTableFlags tf = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
  if (homeLoading) {
    // throbber in place of the listing while a folder loads
    BeginCard("##homeloading");
    ImGui::Dummy(ImVec2(0, S(24)));
    std::string msg = "Loading " + (homePath.empty() ? baseName(repo) : homePath) + "\xE2\x80\xA6";
    float w = S(24) + S(10) + ImGui::CalcTextSize(msg.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (ImGui::GetContentRegionAvail().x - w) / 2));
    Spinner(S(12), S(2.5f));
    ImGui::SameLine(0, S(10));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(12) - ImGui::GetTextLineHeight() / 2);
    Muted(msg);
    ImGui::Dummy(ImVec2(0, S(24)));
    EndCard();
  } else if (ImGui::BeginTable("##files", 3, tf)) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
    ImGui::TableSetupColumn("Last commit", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("Last update", ImGuiTableColumnFlags_WidthStretch, 0.2f);
    ImGui::PushFont(F.bold); ImGui::TableHeadersRow(); ImGui::PopFont();
    if (!homePath.empty()) {
      ImGui::TableNextRow(); ImGui::TableNextColumn();
      if (ImGui::Selectable("..", false, ImGuiSelectableFlags_SpanAllColumns)) {
        auto s = homePath.rfind('/'); navTo = s == std::string::npos ? "" : homePath.substr(0, s);
      }
    }
    for (size_t i = 0; i < d.entries.size(); i++) {
      auto &e = d.entries[i];
      ImGui::TableNextRow(); ImGui::TableNextColumn();
      ImGui::PushID((int)i);
      ImGui::PushStyleColor(ImGuiCol_Text, e.dir ? C.accent : C.muted);
      ImGui::TextUnformatted(e.dir ? "\xE2\x96\xB6" : "\xE2\x96\xA1");   // folder / file
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::Selectable(e.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
        std::string full = (homePath.empty() ? "" : homePath + "/") + e.name;
        if (e.dir) navTo = full;
        else if (ImGui::IsMouseDoubleClicked(0)) openPath(rp + "/" + full);
      }
      if (!e.dir && ImGui::IsItemHovered()) ImGui::SetTooltip("Double-click to open");
      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_Text, C.muted);
      if (e.subject.empty() && !e.time && homeDetails && i < 300) Spinner(S(5), S(1.5f));
      else ImGui::TextUnformatted(e.subject.c_str());
      ImGui::TableNextColumn();
      if (e.time) {
        std::string a = ago(e.time);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(a.c_str()).x);
        ImGui::TextUnformatted(a.c_str());
      }
      ImGui::PopStyleColor();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (navTo && (*navTo != homePath || !homeLoading)) { homePath = *navTo; loadHome(true); }
  if (!homeLoading && d.entries.empty()) Muted(d.lastHash.empty() ? "This repository has no commits yet." : "Empty folder.");

  // README
  if (!homeLoading && !d.readme.empty()) {
    ImGui::Spacing(); ImGui::Spacing();
    BeginCard("##readme");
    ImGui::PushFont(F.bold); ImGui::TextUnformatted(d.readmeName.c_str()); ImGui::PopFont();
    ImGui::Separator(); ImGui::Spacing();
    if (endsWith(lower(d.readmeName), ".md") || endsWith(lower(d.readmeName), ".markdown")) Markdown(d.readme);
    else { ImGui::PushFont(F.mono); ImGui::TextWrapped("%s", d.readme.c_str()); ImGui::PopFont(); }
    EndCard();
  }
  ImGui::EndChild();
  ImGui::EndGroup();

  // project information
  if (side) ImGui::SameLine(0, S(20));
  else ImGui::Spacing();
  ImGui::BeginGroup();
  ImGui::BeginChild("##homeside", ImVec2(side ? sideW : 0, 0), ImGuiChildFlags_AutoResizeY);
  ImGui::PushFont(F.bold); ImGui::TextUnformatted("Project information"); ImGui::PopFont();
  ImGui::Spacing();
  // language bar
  long long total = 0; for (auto &l : d.langs) total += l.second;
  if (total > 0) {
    ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = S(8), x = p.x;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(C.border), h / 2);
    for (size_t i = 0; i < d.langs.size(); i++) {
      float seg = w * (float)d.langs[i].second / (float)total;
      dl->AddRectFilled(ImVec2(x, p.y), ImVec2(i + 1 == d.langs.size() ? p.x + w : x + seg, p.y + h), col(langColor(d.langs[i].first)),
                        h / 2, i == 0 ? ImDrawFlags_RoundCornersLeft : i + 1 == d.langs.size() ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
      x += seg;
    }
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, h));
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      for (auto &l : d.langs) ImGui::Text("%s  %.1f%%", l.first.c_str(), 100.0 * l.second / total);
      ImGui::EndTooltip();
    }
    ImGui::Spacing();
  }
  ImGui::Separator(); ImGui::Spacing();
  StatRow("\xE2\x97\x8B", std::to_string(d.commits), d.commits == 1 ? "Commit" : "Commits");
  StatRow("\xE2\x91\x82", std::to_string(d.branches), d.branches == 1 ? "Branch" : "Branches");
  StatRow("\xE2\x97\x87", std::to_string(d.tags), d.tags == 1 ? "Tag" : "Tags");
  StatRow("\xE2\x96\xA4", humanSize(d.storage), "Project Storage");
  if (Button("Branches", false, false, ImVec2(-1, 0))) tab = Tab::Branches;
  if (Button("Open folder", false, false, ImVec2(-1, 0))) showInExplorer(rp);

  if (total > 0) {
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::PushFont(F.bold); ImGui::TextUnformatted("Languages"); ImGui::PopFont();
    for (auto &l : d.langs) {
      double pct = 100.0 * l.second / total;
      if (pct < 0.1) continue;
      ImVec2 p = ImGui::GetCursorScreenPos(); float r = S(4.5f);
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() / 2), r, col(langColor(l.first)));
      ImGui::Dummy(ImVec2(r * 2, 0)); ImGui::SameLine();
      ImGui::PushFont(F.bold); ImGui::TextUnformatted(l.first.c_str()); ImGui::PopFont();
      ImGui::SameLine();
      char b[16]; snprintf(b, sizeof b, "%.1f%%", pct); Muted(b);
    }
  }
  if (d.created) {
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::PushFont(F.bold); ImGui::TextUnformatted("Created on"); ImGui::PopFont();
    ImGui::TextUnformatted(formatTime(d.created).c_str());
  }
  ImGui::EndChild();
  ImGui::EndGroup();
  ImGui::EndChild();
}
