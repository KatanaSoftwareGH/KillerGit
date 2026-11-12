// Console tab: run git commands in the current repository, either typed by hand or from a template
// whose {placeholders} become input fields. Custom templates are saved in settings ("consoleTemplates").
#include "app.h"
#include <algorithm>

static float S(float v) { return v * ImGui::GetFontSize() / 15.5f; }

namespace {
struct Template { std::string name, cmd, hint; bool danger = false, custom = false; };

// {name} is required, {name=default} is pre-filled.
const std::vector<Template> &builtins() {
  static const std::vector<Template> t = {
    { "Undo last commit (keep changes)", "reset --soft HEAD~1", "Moves HEAD back one commit; the changes stay staged." },
    { "Undo last N commits (keep changes)", "reset --soft HEAD~{count=1}", "Moves HEAD back N commits; the changes stay staged." },
    { "Amend last commit message", "commit --amend -m \"{message}\"", "Rewrites the last commit's message. Don't do this to pushed commits." },
    { "Tag and push release", "tag {tag=v0.2.0} && push {remote=Github} {tag=v0.2.0}", "Creates the tag, then pushes it. With the GitHub remote, a v* tag triggers the release workflow." },
    { "Create and switch to branch", "switch -c {name}", "" },
    { "Create branch from commit", "switch -c {name} {commit}", "" },
    { "Switch branch", "switch {branch}", "" },
    { "Set upstream and push", "push -u {remote=origin} {branch}", "" },
    { "Force push (with lease)", "push --force-with-lease {remote=origin} {branch}", "Overwrites the remote branch only if nobody else pushed to it.", true },
    { "Delete remote branch", "push {remote=origin} --delete {branch}", "", true },
    { "Stash everything (incl. untracked)", "stash push -u -m \"{message}\"", "" },
    { "Restore a file from a commit", "restore --source={commit=HEAD} -- {file}", "" },
    { "Discard changes to a file", "restore -- {file}", "Local edits to the file are lost.", true },
    { "Show a file's history", "log --follow --oneline -- {file}", "" },
    { "Search commit messages", "log --oneline -i --grep=\"{text}\"", "" },
    { "Search code history for text", "log --oneline -S\"{text}\"", "Commits that added or removed this text." },
    { "Who changed each line", "blame -- {file}", "" },
    { "Preview untracked files to clean", "clean -nd", "Dry run: lists what clean would delete." },
    { "Delete untracked files", "clean -fd", "Permanently deletes untracked files and folders.", true },
    { "Prune merged local branches", "branch --merged", "Lists branches already merged into the current one." },
    { "Rewrite author of last commit", "commit --amend --no-edit --author=\"{author}\"", "Author format: Name <email>. Don't do this to pushed commits." },
    { "Squash last N commits", "reset --soft HEAD~{count=2}", "Then commit again to get a single commit." },
    { "Reflog (recover lost commits)", "reflog -n {count=20}", "" },
  };
  return t;
}

Strings tokenize(const std::string &s) {
  Strings out; std::string cur; bool has = false; char q = 0;
  for (char c : s) {
    if (q) { if (c == q) q = 0; else cur += c; }
    else if (c == '"' || c == '\'') { q = c; has = true; }
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (has || !cur.empty()) { out.push_back(cur); cur.clear(); has = false; } }
    else cur += c;
  }
  if (has || !cur.empty()) out.push_back(cur);
  return out;
}

struct Param { std::string name, def; };
std::vector<Param> paramsOf(const std::string &cmd) {
  std::vector<Param> ps;
  for (size_t i = 0; (i = cmd.find('{', i)) != std::string::npos;) {
    size_t e = cmd.find('}', i); if (e == std::string::npos) break;
    std::string in = cmd.substr(i + 1, e - i - 1), n = in, d; size_t eq = in.find('=');
    if (eq != std::string::npos) { n = in.substr(0, eq); d = in.substr(eq + 1); }
    if (!n.empty() && std::none_of(ps.begin(), ps.end(), [&](const Param &p) { return p.name == n; })) ps.push_back({ n, d });
    i = e + 1;
  }
  return ps;
}

// Replaces {name} / {name=default} inside a single (already tokenized) argument.
std::string fill(std::string a, const std::map<std::string, std::string> &v) {
  for (size_t i = 0; (i = a.find('{', i)) != std::string::npos;) {
    size_t e = a.find('}', i); if (e == std::string::npos) break;
    std::string in = a.substr(i + 1, e - i - 1), n = in.substr(0, in.find('='));
    auto it = v.find(n); std::string val = it == v.end() ? "" : it->second;
    a.replace(i, e - i + 1, val); i += val.size();
  }
  return a;
}

std::string shown(const Strings &args) {
  std::string s = "git";
  for (auto &a : args) s += a == "&&" ? " && git" : a.find_first_of(" \t\"") != std::string::npos ? " \"" + a + "\"" : " " + a;
  return s;
}
}  // namespace

// Runs one git command, or several joined by "&&" (stops at the first failure).
void App::consoleRun(const std::vector<std::string> &argsIn) {
  if (conRunning || repo.empty()) return;
  std::vector<Strings> cmds(1);
  for (auto &a : argsIn) { if (a == "&&") cmds.emplace_back(); else cmds.back().push_back(a); }
  for (auto &c : cmds) if (!c.empty() && (c[0] == "git" || c[0] == "git.exe")) c.erase(c.begin());
  cmds.erase(std::remove_if(cmds.begin(), cmds.end(), [](const Strings &c) { return c.empty(); }), cmds.end());
  if (cmds.empty()) return;
  conRunning = true;
  std::string rp = repo, cur = currentRemote();
  std::map<std::string, std::string> urls;  // remote name -> URL, so network commands use that remote's credentials
  for (auto &r : remotes) urls[r.name] = r.fetch;
  std::string defUrl = urls.count(cur) ? urls[cur] : "";
  bg([=] {
    for (auto &args : cmds) {
      std::string u = defUrl;
      if (args[0] == "push" || args[0] == "pull" || args[0] == "fetch") for (auto &a : args) if (urls.count(a)) u = urls.at(a);
      ui([=] { conLog += "$ " + shown(args) + "\n"; conScroll = true; });
      ProcResult r = git::runRaw(rp, args, u);
      std::string o = r.out + (r.err.empty() ? "" : (r.out.empty() || r.out.back() == '\n' ? "" : "\n") + r.err);
      while (!o.empty() && (o.back() == '\n' || o.back() == '\r')) o.pop_back();
      ui([=] { conLog += (o.empty() ? "" : o + "\n") + (r.code == 0 ? "" : "[exit " + std::to_string(r.code) + "]\n"); conScroll = true; });
      if (r.code != 0) break;
    }
    ui([=] { conRunning = false; conLog += "\n"; conScroll = true; if (repo == rp) refreshRepo(); });
  });
}

void App::viewConsole() {
  json custom = settings.value("consoleTemplates", json::array());
  std::vector<Template> all = builtins();
  for (auto &c : custom) if (c.is_object()) all.push_back({ c.value("name", ""), c.value("cmd", ""), "", false, true });

  // --- templated workflow ---
  BeginCard("##tpl");
  ImGui::PushFont(F.bold); ImGui::TextUnformatted("Template"); ImGui::PopFont();
  ImGui::SetNextItemWidth(S(360));
  const char *cur = conTpl >= 0 && conTpl < (int)all.size() ? all[conTpl].name.c_str() : "Choose a template…";
  if (ImGui::BeginCombo("##tplc", cur)) {
    for (int i = 0; i < (int)all.size(); i++) {
      if (i == (int)builtins().size() && !custom.empty()) ImGui::Separator();
      if (ImGui::Selectable((all[i].name + "##t" + std::to_string(i)).c_str(), conTpl == i)) {
        conTpl = i; conParams.clear();
        for (auto &p : paramsOf(all[i].cmd)) {
          std::string d = p.def;
          if (p.name == "branch" && status) d = status->branch;
          conParams[p.name] = d;
        }
      }
    }
    ImGui::EndCombo();
  }
  if (conTpl >= 0 && conTpl < (int)all.size()) {
    const Template &t = all[conTpl];
    if (!t.hint.empty()) Muted(t.hint);
    bool ready = true;
    for (auto &p : paramsOf(t.cmd)) {
      ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(p.name.c_str()); ImGui::SameLine(S(110));
      ImGui::SetNextItemWidth(S(360));
      ImGui::InputText(("##p" + p.name).c_str(), &conParams[p.name]);
      if (trim(conParams[p.name]).empty()) ready = false;
    }
    Strings args;
    for (auto &a : tokenize(t.cmd)) args.push_back(fill(a, conParams));
    ImGui::PushFont(F.mono); ImGui::TextDisabled("%s", shown(args).c_str()); ImGui::PopFont();
    ImGui::BeginDisabled(!ready || conRunning);
    if (Button("Run template", true, t.danger)) {
      if (t.danger) confirm(t.name, shown(args) + "\n\nThis can't be undone.", "Run", true, [=] { consoleRun(args); });
      else consoleRun(args);
    }
    ImGui::EndDisabled();
    if (t.custom) {
      ImGui::SameLine();
      if (Button("Delete template", false, true)) {
        int idx = conTpl - (int)builtins().size(); json next = json::array();
        for (int i = 0; i < (int)custom.size(); i++) if (i != idx) next.push_back(custom[i]);
        saveSettings({ { "consoleTemplates", next } }); conTpl = -1;
      }
    }
  } else {
    Muted("Pick a template to fill in its fields and run it, or type a command below. Use {name} or {name=default} in your own templates.");
  }
  EndCard();

  // --- output ---
  float inputH = ImGui::GetFrameHeight() + S(14);
  ImGui::BeginChild("##conout", ImVec2(0, -inputH), ImGuiChildFlags_Borders);
  ImGui::PushFont(F.mono);
  if (conLog.empty()) ImGui::TextDisabled("Output appears here.");
  else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(conLog.c_str()); ImGui::PopTextWrapPos(); }
  ImGui::PopFont();
  if (conScroll) { ImGui::SetScrollHereY(1.0f); conScroll = false; }
  ImGui::EndChild();

  // --- manual workflow ---
  ImGui::AlignTextToFramePadding(); ImGui::PushFont(F.mono); ImGui::TextUnformatted("git"); ImGui::PopFont(); ImGui::SameLine();
  float btns = S(250);
  ImGui::SetNextItemWidth(-btns);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  bool enter = ImGui::InputTextWithHint("##conin", "status, log --oneline -5, push … (Enter to run, Up for history)", &conInput, ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemActive() && !conHistory.empty()) {
    bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if (up) conHistPos = conHistPos < 0 ? (int)conHistory.size() - 1 : std::max(0, conHistPos - 1);
    if (down && conHistPos >= 0) conHistPos++;
    if (up || down) {
      if (conHistPos >= (int)conHistory.size()) { conHistPos = -1; conInput.clear(); }
      else conInput = conHistory[conHistPos];
    }
  }
  bool run = enter;
  ImGui::SameLine(); ImGui::BeginDisabled(conRunning || trim(conInput).empty());
  if (Button("Run", true)) run = true;
  ImGui::EndDisabled();
  ImGui::SameLine(); ImGui::BeginDisabled(trim(conInput).empty());
  if (Button("Save as template")) {
    std::string cmd = trim(conInput);
    prompt("Save template", "Template name", "", [=](std::string n) {
      json c = settings.value("consoleTemplates", json::array()); c.push_back({ { "name", n }, { "cmd", cmd } });
      saveSettings({ { "consoleTemplates", c } });
    });
  }
  ImGui::EndDisabled();
  ImGui::SameLine(); if (Button("Clear")) conLog.clear();
  if (run && !trim(conInput).empty() && !conRunning) {
    std::string line = trim(conInput);
    if (conHistory.empty() || conHistory.back() != line) conHistory.push_back(line);
    conHistPos = -1; conInput.clear();
    consoleRun(tokenize(line));
    ImGui::SetKeyboardFocusHere(-1);
  }
}
