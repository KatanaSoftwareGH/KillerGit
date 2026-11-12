#include "app.h"
#include <algorithm>
#include <ctime>
#include <map>
#include <set>

static float S(float v) { return v * ImGui::GetFontSize() / 15.5f; }

static void Label(const char *t) { ImGui::PushStyleColor(ImGuiCol_Text, C.muted); ImGui::PushFont(F.small); ImGui::TextUnformatted(t); ImGui::PopFont(); ImGui::PopStyleColor(); }
static bool Field(const char *label, const char *id, std::string *v, const char *hint = "", ImGuiInputTextFlags flags = 0) {
  Label(label); ImGui::SetNextItemWidth(-1); return ImGui::InputTextWithHint(id, hint, v, flags);
}
// Right-aligned Cancel + primary button row. Returns 1 for primary, -1 for cancel, 0 otherwise.
static int Actions(const char *primary, bool danger = false, bool disabled = false) {
  ImGui::Spacing();
  float bw = S(110), w = ImGui::GetContentRegionAvail().x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - bw * 2 - S(8));
  if (Button("Cancel", false, false, ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) return -1;
  ImGui::SameLine();
  ImGui::BeginDisabled(disabled);
  bool r = Button(primary, true, danger, ImVec2(bw, 0));
  ImGui::EndDisabled();
  return r ? 1 : 0;
}
static bool FolderField(const char *label, const char *id, std::string *v) {
  Label(label);
  ImGui::SetNextItemWidth(-S(100)); ImGui::InputText(id, v);
  ImGui::SameLine();
  if (Button((std::string("Choose…##") + id).c_str(), false, false, ImVec2(-1, 0))) { std::string d = pickFolder(label); if (!d.empty()) { *v = d; return true; } }
  return false;
}
static std::string acctLabel(const json &a) {
  std::string l = a.value("label", ""); if (l.empty()) l = a.value("username", ""); if (l.empty()) l = a.value("provider", "");
  return l + " — " + a.value("host", "");
}

static std::string defaultCloneDir() { return app.settings.value("cloneDir", documentsDir() + KG_SEP "GitHub"); }

// ---------------- identity ----------------
void App::ensureIdentity(std::function<void()> then) {
  std::string rp = repo;
  bg([=] {
    std::string n = git::config(rp, "user.name"), e = git::config(rp, "user.email");
    ui([=] {
      if (!n.empty() && !e.empty()) { then(); return; }
      auto name = std::make_shared<std::string>(n.empty() ? settings.value("gitName", "") : n);
      auto email = std::make_shared<std::string>(e.empty() ? settings.value("gitEmail", "") : e);
      modal("Who are you?", [=] {
        Muted("Git records a name and email on every commit. This is saved globally and can be changed in Settings.");
        Field("Name", "##n", name.get());
        Field("Email", "##e", email.get(), "Use the email of your GitHub/GitLab account");
        int a = Actions("Save", false, trim(*name).empty() || trim(*email).empty());
        if (a < 0) return false;
        if (a > 0) {
          std::string nn = trim(*name), ee = trim(*email);
          saveSettings({ { "gitName", nn }, { "gitEmail", ee } });
          bg([=] { try { git::setConfig("", "user.name", nn); git::setConfig("", "user.email", ee); ui(then); }
                   catch (std::exception &x) { std::string m = x.what(); ui([=] { toast(m, true); }); } });
          return false;
        }
        return true;
      });
    });
  });
}

// ---------------- add / clone / create ----------------
void App::addLocal() {
  std::string dir = pickFolder("Choose a repository folder");
  if (dir.empty()) return;
  bg([=] {
    bool ok = git::isRepo(dir);
    ui([=] {
      if (ok) { addRepoPath(dir); return; }
      confirm("Not a repository", "That folder is not a Git repository. Initialize one there?", "Initialize", false, [=] {
        bg([=] { try { git::init(dir); ui([=] { addRepoPath(dir); }); } catch (std::exception &e) { std::string m = e.what(); ui([=] { toast(m, true); }); } });
      });
    });
  });
}

void App::cloneTo(const std::string &url, const std::string &dir, int depth) {
  std::string name = url;
  while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
  if (endsWith(name, ".git")) name.resize(name.size() - 4);
  name = name.substr(name.find_last_of("/:") + 1);
  std::string dest = dir + KG_SEP + name;
  task("Cloning " + name, [=](const Progress &p) {
    git::clone(url, dest, p, depth);
    ui([=] { addRepoPath(dest); });
  }, false);
}

struct CloneState {
  std::string url, dir, filter; int tab = -1; bool shallow = false; int depth = 50; bool ssh = true;
  std::vector<providers::HostRepo> repos; bool loading = false; std::string error; int sel = -1;
};

void App::cloneDialog() {
  auto s = std::make_shared<CloneState>();
  s->dir = defaultCloneDir();
  json accounts = vault::accounts();
  modal("Clone a repository", [=] {
    // Source tabs: URL + one per account.
    if (Pill("URL##ct", s->tab < 0)) s->tab = -1;
    for (int i = 0; i < (int)accounts.size(); i++) {
      ImGui::SameLine();
      auto &a = accounts[i];
      std::string p = a.value("provider", ""), n = p == "github" ? "GitHub" : p == "gitgud" ? "Gitgud" : p == "gitlab" ? "GitLab" : a.value("host", "");
      if (!a.value("username", "").empty()) n += " \xC2\xB7 " + a.value("username", "");
      if (Pill((n + "##ct" + std::to_string(i)).c_str(), s->tab == i) && s->tab != i) {
        s->tab = i; s->repos.clear(); s->error.clear(); s->loading = true; s->sel = -1;
        std::string id = a.value("id", "");
        bg([=] {
          try { auto r = providers::repos(id); ui([=] { if (s->tab == i) { s->repos = r; s->loading = false; } }); }
          catch (std::exception &e) { std::string m = e.what(); ui([=] { s->error = m; s->loading = false; }); }
        });
      }
    }
    ImGui::Spacing();
    if (s->tab < 0) Field("Repository URL", "##url", &s->url, "https://… or git@host:namespace/repo.git");
    else {
      ImGui::SetNextItemWidth(-S(120)); ImGui::InputTextWithHint("##f", "Filter", &s->filter);
      ImGui::SameLine();
      if (ImGui::RadioButton("SSH", s->ssh)) s->ssh = true;
      ImGui::SameLine();
      if (ImGui::RadioButton("HTTPS", !s->ssh)) s->ssh = false;
      ImGui::PushStyleColor(ImGuiCol_ChildBg, C.card);
      ImGui::BeginChild("##rl", ImVec2(0, S(280)), ImGuiChildFlags_Borders);
      if (s->loading) { Spinner(S(8), S(2)); ImGui::SameLine(); Muted("Loading repositories…"); }
      if (!s->error.empty()) Muted(s->error);
      std::string lastNs;
      std::vector<int> order; for (int i = 0; i < (int)s->repos.size(); i++) if (icontains(s->repos[i].fullPath, s->filter)) order.push_back(i);
      std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return s->repos[a].ns < s->repos[b].ns; });
      for (int i : order) {
        auto &r = s->repos[i];
        if (r.ns != lastNs) { lastNs = r.ns; ImGui::PushFont(F.small); ImGui::TextDisabled("%s", r.ns.c_str()); ImGui::PopFont(); }
        ImGui::PushID(i);
        float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable(("   " + r.name).c_str(), s->sel == i)) s->sel = i;
        ImGui::SameLine(rightEdge - S(150));
        ImGui::TextDisabled("%s  %s", r.isPrivate ? "private" : "public", ago(r.updated).c_str());
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
      if (s->sel >= 0) s->url = s->ssh ? s->repos[s->sel].sshUrl : s->repos[s->sel].httpsUrl;
    }
    FolderField("Clone into folder", "##dir", &s->dir);
    ImGui::Checkbox("Shallow clone (good for slow connections) — depth", &s->shallow);
    ImGui::SameLine(); ImGui::SetNextItemWidth(S(70)); ImGui::InputInt("##depth", &s->depth, 0);
    int a = Actions("Clone", false, trim(s->url).empty());
    if (a < 0) return false;
    if (a > 0) {
      if (s->dir != settings.value("cloneDir", "")) saveSettings({ { "cloneDir", s->dir } });
      cloneTo(trim(s->url), s->dir, s->shallow ? std::max(1, s->depth) : 0);
      return false;
    }
    return true;
  }, S(640));
}

static const std::map<std::string, std::string> IGNORES = {
  { "None", "" }, { "Node", "node_modules/\ndist/\n.env\n*.log\n" }, { "Python", "__pycache__/\n*.py[cod]\n.venv/\n.env\ndist/\n*.egg-info/\n" },
  { "VisualStudio", ".vs/\nbin/\nobj/\n*.user\n*.suo\n" }, { "Java", "*.class\ntarget/\nbuild/\n.gradle/\n.idea/\n" },
  { "Go", "bin/\n*.exe\n*.test\nvendor/\n" }, { "Rust", "target/\n" }, { "C++", "build/\nout/\n*.obj\n*.o\n*.exe\n*.pdb\n.vs/\n" },
  { "Unity", "[Ll]ibrary/\n[Tt]emp/\n[Oo]bj/\n[Bb]uild/\n[Ll]ogs/\n*.csproj\n*.sln\n" },
};
static std::string licenseText(const std::string &kind, const std::string &who) {
  std::time_t t = std::time(nullptr); std::tm tm{}; localTime(t, tm);
  if (kind == "MIT") return "MIT License\n\nCopyright (c) " + std::to_string(tm.tm_year + 1900) + " " + (who.empty() ? "the authors" : who) +
    "\n\nPermission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\n\n"
    "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n";
  if (kind == "Unlicense") return "This is free and unencumbered software released into the public domain.\n\nFor more information, please refer to <https://unlicense.org>\n";
  return "";
}

void App::createDialog() {
  ensureIdentity([this] {
    struct St { std::string name, dir, branch = "main"; bool readme = true, publish = false; int ignore = 0, license = 0; };
    auto s = std::make_shared<St>(); s->dir = defaultCloneDir();
    static const char *ign[] = { "None", "Node", "Python", "VisualStudio", "Java", "Go", "Rust", "C++", "Unity" };
    static const char *lic[] = { "None", "MIT", "Unlicense" };
    modal("Create a new repository", [=] {
      Field("Name", "##n", &s->name);
      FolderField("Local path", "##d", &s->dir);
      Field("Default branch", "##b", &s->branch);
      ImGui::Checkbox("Initialize with a README", &s->readme);
      Label("Git ignore"); ImGui::SetNextItemWidth(-1); ImGui::Combo("##i", &s->ignore, ign, IM_ARRAYSIZE(ign));
      Label("License"); ImGui::SetNextItemWidth(-1); ImGui::Combo("##l", &s->license, lic, IM_ARRAYSIZE(lic));
      ImGui::Checkbox("Also create it on a hosting service afterwards", &s->publish);
      int a = Actions("Create", false, trim(s->name).empty());
      if (a < 0) return false;
      if (a > 0) {
        std::string dir = s->dir; while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
        std::string path = dir + KG_SEP + trim(s->name), branch = s->branch.empty() ? "main" : s->branch;
        std::string gi = IGNORES.at(ign[s->ignore]), li = licenseText(lic[s->license], settings.value("gitName", ""));
        bool readme = s->readme, pub = s->publish;
        task("Creating repository", [=](const Progress &) {
          makeDirs(path);
          git::init(path, branch);
          if (readme) writeFile(path + KG_SEP "README.md", "# " + baseName(path) + "\n");
          if (!gi.empty()) writeFile(path + KG_SEP ".gitignore", gi);
          if (!li.empty()) writeFile(path + KG_SEP "LICENSE", li);
          if (readme || !gi.empty() || !li.empty()) { git::stageAll(path); git::commit(path, "Initial commit"); }
          ui([=] {
            addRepoPath(path);
            if (pub) publishDialog(true, false);
          });
        }, false);
        return false;
      }
      return true;
    }, S(520));
  });
}

void App::addRemoteDialog() {
  auto name = std::make_shared<std::string>(remotes.empty() ? "origin" : ""), url = std::make_shared<std::string>();
  std::string rp = repo; auto existing = remotes;
  modal("Add remote", [=] {
    Field("Name", "##n", name.get(), "e.g. gitgud");
    Field("URL", "##u", url.get(), "git@host:namespace/repo.git or https://…");
    int a = Actions("Add", false, trim(*name).empty() || trim(*url).empty());
    if (a < 0) return false;
    if (a > 0) {
      std::string n = trim(*name), u = trim(*url);
      bool exists = std::any_of(existing.begin(), existing.end(), [&](auto &r) { return r.name == n; });
      if (exists) task("Updating " + n + " URL", [=](const Progress &) { git::setRemoteUrl(rp, n, u); });
      else task("Adding remote", [=](const Progress &) { git::addRemote(rp, n, u); });
      return false;
    }
    return true;
  });
}

void App::publishDialog(bool forLocal, bool cloneAfter) {
  json accounts = vault::accounts();
  if (accounts.empty()) { toast("Add an account first (Accounts)."); accountsPage(); return; }
  struct St { int acct = 0; std::vector<providers::Namespace> ns; int nsSel = 0; bool nsLoading = false; int loadedFor = -1;
              std::string name, desc, remoteName; bool priv = true, ssh = true, push = true; };
  auto s = std::make_shared<St>();
  std::string localPath = forLocal ? repo : "";
  s->name = forLocal ? baseName(repo) : "";
  s->remoteName = std::any_of(remotes.begin(), remotes.end(), [](auto &r) { return r.name == "origin"; }) ? "" : "origin";
  std::string branch = status ? status->branch : "";
  modal(forLocal ? "Create repository on host & add as remote" : "Create repository on host", [=] {
    Label("Account"); ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##a", acctLabel(accounts[s->acct]).c_str())) {
      for (int i = 0; i < (int)accounts.size(); i++) if (ImGui::Selectable(acctLabel(accounts[i]).c_str(), i == s->acct)) s->acct = i;
      ImGui::EndCombo();
    }
    if (s->loadedFor != s->acct) {
      s->loadedFor = s->acct; s->nsLoading = true; s->ns.clear(); s->nsSel = 0;
      std::string id = accounts[s->acct].value("id", ""); int forA = s->acct;
      bg([=] {
        std::vector<providers::Namespace> ns;
        try { ns = providers::namespaces(id); } catch (...) { ns = { { "", "Personal", "user" } }; }
        ui([=] { if (s->acct == forA) { s->ns = ns; s->nsLoading = false; } });
      });
    }
    Label("Owner / namespace"); ImGui::SetNextItemWidth(-1);
    std::string nsLabel = s->nsLoading ? "Loading…" : s->ns.empty() ? "Personal" : s->ns[s->nsSel].path + " (" + s->ns[s->nsSel].kind + ")";
    if (ImGui::BeginCombo("##ns", nsLabel.c_str())) {
      for (int i = 0; i < (int)s->ns.size(); i++) if (ImGui::Selectable((s->ns[i].path + " (" + s->ns[i].kind + ")").c_str(), i == s->nsSel)) s->nsSel = i;
      ImGui::EndCombo();
    }
    Field("Name", "##name", &s->name);
    Field("Description", "##desc", &s->desc);
    ImGui::Checkbox("Private repository", &s->priv);
    ImGui::SameLine(); ImGui::TextDisabled("   Protocol"); ImGui::SameLine();
    if (ImGui::RadioButton("SSH", s->ssh)) s->ssh = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("HTTPS", !s->ssh)) s->ssh = false;
    if (!localPath.empty()) {
      Field("Local remote name", "##rn", &s->remoteName, "e.g. github, gitgud");
      ImGui::Checkbox("Push current branch after creating", &s->push);
    }
    int a = Actions("Create", false, trim(s->name).empty() || s->nsLoading);
    if (a < 0) return false;
    if (a > 0) {
      json acct = accounts[s->acct];
      std::string id = acct.value("id", ""), name = trim(s->name), desc = s->desc, ns = s->ns.empty() ? "" : s->ns[s->nsSel].id;
      std::string rn = trim(s->remoteName).empty() ? acct.value("provider", "origin") : trim(s->remoteName);
      bool priv = s->priv, ssh = s->ssh, push = s->push;
      task("Creating repository", [=](const Progress &p) {
        auto r = providers::createRepo(id, name, desc, priv, ns);
        std::string url = ssh ? r.sshUrl : r.httpsUrl;
        if (!localPath.empty()) {
          git::addRemote(localPath, rn, url);
          if (push && !branch.empty()) { git::PushOpts o; o.setUpstream = true; git::push(localPath, rn, branch, o, p); }
        }
        ui([=] {
          toast("Created " + r.web);
          if (cloneAfter) cloneTo(url, defaultCloneDir(), 0);
        });
      });
      return false;
    }
    return true;
  }, S(560));
}

void App::importGithubDesktop() {
  struct St { Strings repos; std::vector<char> checked; bool loading = true; std::string filter; };
  auto s = std::make_shared<St>();
  bg([=] {
    Strings found;
    try { found = githubDesktopRepos(); } catch (...) {}
    ui([=] {
      Strings existing = settings.value("repos", Strings{});
      std::set<std::string> ex(existing.begin(), existing.end());
      s->repos = found; std::sort(s->repos.begin(), s->repos.end());
      for (auto &p : s->repos) s->checked.push_back(!ex.count(p));
      s->loading = false;
      if (found.empty()) toast("No repositories found in GitHub Desktop. Make sure GitHub Desktop has been used on this machine.");
    });
  });
  modal("Import from GitHub Desktop", [=] {
    if (s->loading) { Spinner(S(10), S(2.5f)); ImGui::SameLine(); Muted("Scanning GitHub Desktop's database and logs…"); return !(ImGui::IsKeyPressed(ImGuiKey_Escape)); }
    if (s->repos.empty()) return false;
    Strings existing = settings.value("repos", Strings{});
    std::set<std::string> ex(existing.begin(), existing.end());
    int sel = 0; for (auto c : s->checked) sel += c;
    bool all = sel == (int)s->repos.size();
    if (ImGui::Checkbox("##all", &all)) for (auto &c : s->checked) c = all;
    ImGui::SameLine(); ImGui::Text("%d of %zu selected", sel, s->repos.size());
    ImGui::SameLine(); ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##f", "Filter repositories…", &s->filter);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C.card);
    ImGui::BeginChild("##l", ImVec2(0, S(340)), ImGuiChildFlags_Borders);
    ImGui::PushFont(F.mono);
    for (size_t i = 0; i < s->repos.size(); i++) {
      if (!icontains(s->repos[i], s->filter)) continue;
      ImGui::PushID((int)i);
      bool c = s->checked[i];
      if (ImGui::Checkbox(s->repos[i].c_str(), &c)) s->checked[i] = c;
      if (ex.count(s->repos[i])) { ImGui::SameLine(); ImGui::TextDisabled("(already added)"); }
      ImGui::PopID();
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    int a = Actions("Import", false, sel == 0);
    if (a < 0) return false;
    if (a > 0) {
      Strings merged = existing; std::string first;
      for (size_t i = 0; i < s->repos.size(); i++) if (s->checked[i] && !ex.count(s->repos[i])) { merged.push_back(s->repos[i]); if (first.empty()) first = s->repos[i]; }
      saveSettings({ { "repos", merged } });
      loadRepos();
      if (repo.empty() && !first.empty()) openRepo(first);
      toast("Imported " + std::to_string(sel) + " repository(ies) from GitHub Desktop");
      return false;
    }
    return true;
  }, S(760));
}

// ---------------- accounts, keys, secrets ----------------
static const std::map<std::string, std::string> SSH_HOSTS = { { "gitgud.io", "ssh.gitgud.io" } };

struct AcctState {
  int tab = 0;
  json accounts; std::vector<ssh::Key> keys; json secrets;
  bool keysLoading = false;
  void reload() { accounts = vault::accounts(); secrets = vault::secrets(); }
};

static void editAccount(json a, std::shared_ptr<AcctState> st) {
  struct E { int provider = 0; std::string host, api, user, label, token, key; };
  auto e = std::make_shared<E>();
  for (int i = 0; i < 4; i++) if (a.value("provider", "") == vault::PRESETS[i].id) e->provider = i;
  e->host = a.value("host", ""); e->api = a.value("api", ""); e->user = a.value("username", ""); e->label = a.value("label", ""); e->key = a.value("sshKey", "");
  bool hasToken = !a.value("token", "").empty();
  auto keys = ssh::list(false);
  app.modal(a.is_object() && a.contains("id") ? "Edit account" : "Add account", [=] {
    Label("Service"); ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##p", vault::PRESETS[e->provider].name)) {
      for (int i = 0; i < 4; i++) if (ImGui::Selectable(vault::PRESETS[i].name, i == e->provider)) e->provider = i;
      ImGui::EndCombo();
    }
    bool custom = std::string(vault::PRESETS[e->provider].id) == "custom";
    if (custom) { Field("Host", "##h", &e->host, "gitlab.example.com"); Field("API URL", "##api", &e->api, "https://gitlab.example.com/api/v4"); }
    Field("Username", "##u", &e->user, "Filled in automatically from the token if empty");
    Field("Label", "##l", &e->label, "e.g. Work");
    Field("Personal access token", "##t", &e->token, hasToken ? "(unchanged)" : "Personal access token", ImGuiInputTextFlags_Password);
    Muted("Token scopes: GitHub \xE2\x86\x92 repo, read:org, admin:public_key. GitLab/Gitgud \xE2\x86\x92 api.");
    Label("SSH key for this host"); ImGui::SetNextItemWidth(-1);
    std::string cur = "Default (ssh-agent / ~/.ssh)";
    for (auto &k : keys) if (k.path == e->key) cur = k.name + " (" + k.type + ")";
    if (ImGui::BeginCombo("##k", cur.c_str())) {
      if (ImGui::Selectable("Default (ssh-agent / ~/.ssh)", e->key.empty())) e->key.clear();
      for (auto &k : keys) if (k.hasPrivate && ImGui::Selectable((k.name + " (" + k.type + ")").c_str(), k.path == e->key)) e->key = k.path;
      ImGui::EndCombo();
    }
    int r = Actions("Save");
    if (r < 0) return false;
    if (r > 0) {
      json in = { { "id", a.value("id", "") }, { "provider", vault::PRESETS[e->provider].id }, { "username", trim(e->user) },
                  { "label", trim(e->label) }, { "token", trim(e->token) }, { "sshKey", e->key } };
      if (custom) { in["host"] = trim(e->host); in["api"] = trim(e->api); }
      else { in["host"] = ""; in["api"] = ""; }
      std::string id = vault::saveAccount(in);
      st->reload();
      if (trim(e->user).empty() && (!trim(e->token).empty() || hasToken)) {
        bg([=] {
          try { auto me = providers::me(id); json up = { { "id", id }, { "username", me.username } }; vault::saveAccount(up); ui([=] { st->reload(); }); } catch (...) {}
        });
      }
      return false;
    }
    return true;
  }, S(520));
}

static void drawAccountsTab(App &A, std::shared_ptr<AcctState> st) {
  if (Button("Add account", true)) editAccount(json::object(), st);
  #ifdef _WIN32
  Muted("Tokens are encrypted with Windows DPAPI. HTTPS remotes use the token; SSH remotes use the assigned key via Windows OpenSSH.");
#else
  Muted("Tokens are encrypted (AES-256-GCM, key in ~/.config/KillerGit, readable only by you). HTTPS remotes use the token; SSH remotes use the assigned key via OpenSSH.");
#endif
  ImGui::Spacing();
  if (st->accounts.empty()) Muted("No accounts yet.");
  for (size_t i = 0; i < st->accounts.size(); i++) {
    json a = st->accounts[i];
    ImGui::PushID((int)i);
    BeginCard("##a");
    const vault::Preset *p = vault::preset(a.value("provider", ""));
    std::string title = (p && std::string(p->id) != "custom" ? p->name : a.value("host", "")) + (a.value("label", "").empty() ? "" : " — " + a.value("label", ""));
    ImGui::PushFont(F.bold); ImGui::TextUnformatted(title.c_str()); ImGui::PopFont();
    std::string key = a.value("sshKey", "");
    Muted((a.value("username", "").empty() ? "(no username)" : a.value("username", "")) + " @ " + a.value("host", "") + " \xC2\xB7 token: " +
          (a.value("token", "").empty() ? "none" : "saved") + " \xC2\xB7 SSH key: " + (key.empty() ? "default" : baseName(key)));
    std::string id = a.value("id", ""), host = a.value("host", "");
    if (Button("Edit")) editAccount(a, st);
    ImGui::SameLine();
    if (Button("Test token")) bg([=] {
      try { auto me = providers::me(id); ui([=] { app.info("Token works", "Signed in to " + host + " as " + me.username + "."); }); }
      catch (std::exception &e) { std::string m = e.what(); ui([=] { app.info("Token test failed", m); }); }
    });
    ImGui::SameLine();
    if (!a.value("token", "").empty() && Button("Reveal token")) A.info("Token — " + acctLabel(a), a.value("token", ""));
    ImGui::SameLine();
    if (Button("Test SSH")) {
      std::string sshHost = SSH_HOSTS.count(host) ? SSH_HOSTS.at(host) : host;
      A.toast("Testing SSH to " + sshHost + "…");
      bg([=] {
        std::string out = ssh::test(sshHost, key);
        bool ok = icontains(out, "welcome") || icontains(out, "successfully authenticated");
        ui([=] { app.info(ok ? "SSH works" : "SSH test failed", "git@" + sshHost + (key.empty() ? " using default keys" : " using " + baseName(key)) + "\n\n" + out); });
      });
    }
    ImGui::SameLine();
    if (Button("Public key")) {
      auto keys = ssh::list(true); std::string text;
      for (auto &k : keys) if (key.empty() || k.path == key) text += k.name + "  " + k.type + "  " + k.fingerprint + "\n" + k.publicKey + "\n\n";
      A.info(key.empty() ? "Keys tried by default" : "SSH key — " + baseName(key), text.empty() ? "No SSH keys found in ~/.ssh." : trim(text));
    }
    if (!key.empty()) {
      ImGui::SameLine();
      if (Button("Upload key to account")) bg([=] {
        try {
          std::string pub; for (auto &k : ssh::list(false)) if (k.path == key) pub = k.publicKey;
          providers::addSshKey(id, "KillerGit " + baseName(key), pub);
          ui([=] { app.info("Key uploaded", baseName(key) + ".pub was added to your " + host + " account."); });
        } catch (std::exception &e) { std::string m = e.what(); ui([=] { app.info("Upload failed", m); }); }
      });
    }
    ImGui::SameLine();
    if (Button("Remove", false, true)) A.confirm("Remove account", "Remove this account and its saved token?", "Remove", true, [=] { vault::removeAccount(id); st->reload(); });
    EndCard();
    ImGui::PopID();
  }
}

static void reloadKeys(std::shared_ptr<AcctState> st) {
  st->keysLoading = true;
  bg([=] { auto k = ssh::list(true); ui([=] { st->keys = k; st->keysLoading = false; }); });
}

static void importKeysFrom(const std::string &folder, std::shared_ptr<AcctState> st) {
  auto found = ssh::scanFolder(folder);
  auto known = ssh::list(false);
  std::set<std::string> names; for (auto &k : known) names.insert(k.name);
  bool external = lower(folder) != lower(ssh::sshDir());
  std::vector<ssh::Key> fresh;
  for (auto &k : found) if (external ? k.hasPrivate : (!names.count(k.name) || k.pubPath.empty())) fresh.push_back(k);
  if (fresh.empty()) { app.toast("No new SSH keys found in " + folder + "."); return; }
  auto checked = std::make_shared<std::vector<char>>(fresh.size(), 1);
  auto assign = std::make_shared<int>(-1);
  json accts = vault::accounts();
  app.modal("Import SSH keys", [=] {
    Muted("Found " + std::to_string(found.size()) + " key(s) in " + folder + ", " + std::to_string(fresh.size()) + " new to KillerGit.");
    for (size_t i = 0; i < fresh.size(); i++) {
      ImGui::PushID((int)i); bool c = (*checked)[i];
      if (ImGui::Checkbox(fresh[i].name.c_str(), &c)) (*checked)[i] = c;
      ImGui::SameLine(); ImGui::TextDisabled("%s %s", fresh[i].type.c_str(), fresh[i].hasPrivate ? "" : "(public only)");
      ImGui::PopID();
    }
    if (!accts.empty()) {
      Label("Assign selected keys to account (optional)"); ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##as", *assign < 0 ? "— None —" : acctLabel(accts[*assign]).c_str())) {
        if (ImGui::Selectable("— None —", *assign < 0)) *assign = -1;
        for (int i = 0; i < (int)accts.size(); i++) if (ImGui::Selectable(acctLabel(accts[i]).c_str(), i == *assign)) *assign = i;
        ImGui::EndCombo();
      }
    }
    int a = Actions("Import");
    if (a < 0) return false;
    if (a > 0) {
      int n = 0;
      for (size_t i = 0; i < fresh.size(); i++) {
        if (!(*checked)[i]) continue;
        auto &k = fresh[i]; std::string path = k.path;
        try {
          if (k.hasPrivate && (external || k.pubPath.empty())) path = ssh::import(k.name, readFile(k.path));
          if (*assign >= 0 && k.hasPrivate) vault::saveAccount({ { "id", accts[*assign].value("id", "") }, { "sshKey", path } });
          n++;
        } catch (std::exception &e) { app.toast(k.name + ": " + e.what(), true); }
      }
      app.toast("Imported " + std::to_string(n) + " SSH key(s)");
      st->reload(); reloadKeys(st);
      return false;
    }
    return true;
  }, S(560));
}

static void drawKeysTab(App &A, std::shared_ptr<AcctState> st) {
  if (Button("Generate key", true)) {
    struct G { std::string name = "id_ed25519_killergit", comment, pass; int type = 0; bool save = true; };
    auto g = std::make_shared<G>(); g->comment = A.settings.value("gitEmail", "");
    A.modal("Generate SSH key", [=] {
      static const char *types[] = { "Ed25519 (recommended)", "RSA 4096", "ECDSA" };
      Field("File name", "##n", &g->name);
      Label("Type"); ImGui::SetNextItemWidth(-1); ImGui::Combo("##t", &g->type, types, 3);
      Field("Comment", "##c", &g->comment);
      Field("Passphrase", "##p", &g->pass, "Optional", ImGuiInputTextFlags_Password);
      ImGui::Checkbox("Store passphrase in KillerGit vault", &g->save);
      int a = Actions("Generate");
      if (a < 0) return false;
      if (a > 0) {
        const char *t[] = { "ed25519", "rsa", "ecdsa" };
        try {
          std::string p = ssh::generate(trim(g->name), t[g->type], g->comment, g->pass);
          if (!g->pass.empty() && g->save) vault::saveSecret({ { "name", "SSH passphrase: " + g->name }, { "kind", "ssh-passphrase" }, { "value", g->pass } });
          app.toast("Key created at " + p); reloadKeys(st); st->reload();
        } catch (std::exception &e) { app.toast(e.what(), true); return true; }
        return false;
      }
      return true;
    });
  }
  ImGui::SameLine();
  if (Button("Import private key")) {
    auto name = std::make_shared<std::string>(), key = std::make_shared<std::string>();
    A.modal("Import private key", [=] {
      Field("Save as", "##n", name.get(), "id_ed25519_work");
      Label("Private key");
      ImGui::PushFont(F.mono); ImGui::InputTextMultiline("##k", key.get(), ImVec2(-1, S(160))); ImGui::PopFont();
      if (Button("Load from file…")) { std::string f = pickFile("Private key file"); if (!f.empty()) { try { *key = readFile(f); if (name->empty()) *name = baseName(f); } catch (...) {} } }
      int a = Actions("Import", false, trim(*name).empty() || trim(*key).empty());
      if (a < 0) return false;
      if (a > 0) { try { ssh::import(trim(*name), *key); reloadKeys(st); } catch (std::exception &e) { app.toast(e.what(), true); return true; } return false; }
      return true;
    }, S(560));
  }
  ImGui::SameLine();
  if (Button("Import keys from folder…")) { std::string f = pickFolder("Select folder containing SSH keys"); if (!f.empty()) importKeysFrom(f, st); }
  ImGui::SameLine();
  if (Button("Scan ~/.ssh")) importKeysFrom(ssh::sshDir(), st);
  ImGui::SameLine();
  if (Button("Agent keys")) bg([] { std::string s = ssh::agentList(); ui([=] { app.info("ssh-agent", s); }); });
#ifdef _WIN32
  Muted("Keys live in %USERPROFILE%\\.ssh and are used through Windows OpenSSH. To use ssh-agent, enable the \"OpenSSH Authentication Agent\" service.");
#else
  Muted("Keys live in ~/.ssh and are used through OpenSSH. ssh-agent is used when SSH_AUTH_SOCK is set (most desktops start one).");
#endif
  ImGui::Spacing();
  if (st->keysLoading) { Spinner(S(8), S(2)); ImGui::SameLine(); Muted("Looking in ~/.ssh…"); }
  else if (st->keys.empty()) Muted("No keys found.");
  for (size_t i = 0; i < st->keys.size(); i++) {
    auto k = st->keys[i];
    ImGui::PushID((int)i);
    BeginCard("##k");
    ImGui::PushFont(F.bold); ImGui::TextUnformatted(k.name.c_str()); ImGui::PopFont();
    ImGui::SameLine(); ImGui::TextDisabled("%s  %s", k.type.c_str(), k.comment.c_str());
    ImGui::PushFont(F.mono); ImGui::TextDisabled("%s", k.fingerprint.c_str()); ImGui::PopFont();
    if (Button("Copy public key", true)) { copyToClipboard(k.publicKey); A.toast("Public key copied"); }
    ImGui::SameLine();
    if (Button("Edit comment")) A.prompt("Edit comment", "Comment", k.comment, [=](std::string c) { try { ssh::setComment(k.path, c); } catch (std::exception &e) { app.toast(e.what(), true); } reloadKeys(st); });
    ImGui::SameLine();
    if (Button("Passphrase")) {
      auto o = std::make_shared<std::string>(), n = std::make_shared<std::string>();
      A.modal("Change passphrase — " + k.name, [=] {
        Field("Current passphrase (blank if none)", "##o", o.get(), "", ImGuiInputTextFlags_Password);
        Field("New passphrase (blank to remove)", "##n", n.get(), "", ImGuiInputTextFlags_Password);
        int a = Actions("Change");
        if (a < 0) return false;
        if (a > 0) { try { ssh::changePassphrase(k.path, *o, *n); app.toast("Passphrase changed"); } catch (std::exception &e) { app.toast(e.what(), true); return true; } return false; }
        return true;
      });
    }
    ImGui::SameLine();
    if (Button("Rename")) A.prompt("Rename key", "New file name", k.name, [=](std::string n) { if (n != k.name) { try { ssh::rename(k.path, n); } catch (std::exception &e) { app.toast(e.what(), true); } reloadKeys(st); } });
    ImGui::SameLine();
    if (Button("Add to agent")) bg([=] { try { std::string r = ssh::addToAgent(k.path); ui([=] { app.toast(r.empty() ? "Added to agent" : r); }); } catch (std::exception &e) { std::string m = e.what(); ui([=] { app.toast(m, true); }); } });
    ImGui::SameLine();
    if (Button("Show file")) showInExplorer(k.path);
    ImGui::SameLine();
    if (Button("Delete", false, true)) A.confirm("Delete key", "Delete " + k.name + " and " + k.name + ".pub? This cannot be undone.", "Delete", true, [=] { ssh::remove(k.path); reloadKeys(st); });
    EndCard();
    ImGui::PopID();
  }
}

static void editSecret(json s, std::shared_ptr<AcctState> st) {
  struct E { std::string name, value; int kind = 0; };
  static const char *kinds[] = { "token", "ssh-passphrase", "password", "note" };
  auto e = std::make_shared<E>(); e->name = s.value("name", "");
  for (int i = 0; i < 4; i++) if (s.value("kind", "") == kinds[i]) e->kind = i;
  bool existing = s.contains("id");
  app.modal(existing ? "Edit secret" : "Add secret", [=] {
    Field("Name", "##n", &e->name);
    Label("Kind"); ImGui::SetNextItemWidth(-1); ImGui::Combo("##k", &e->kind, kinds, 4);
    Label("Value");
    ImGui::PushFont(F.mono); ImGui::InputTextMultiline("##v", &e->value, ImVec2(-1, S(110))); ImGui::PopFont();
    if (existing && e->value.empty()) Muted("Leave empty to keep the current value.");
    int a = Actions("Save", false, trim(e->name).empty());
    if (a < 0) return false;
    if (a > 0) { vault::saveSecret({ { "id", s.value("id", "") }, { "name", e->name }, { "kind", kinds[e->kind] }, { "value", e->value } }); st->reload(); return false; }
    return true;
  });
}

static void drawSecretsTab(App &A, std::shared_ptr<AcctState> st) {
  if (Button("Add secret", true)) editSecret(json::object(), st);
  Muted("Store extra personal access tokens, deploy keys, passphrases or notes. Encrypted at rest with DPAPI.");
  ImGui::Spacing();
  if (st->secrets.empty()) Muted("Nothing stored.");
  for (size_t i = 0; i < st->secrets.size(); i++) {
    json s = st->secrets[i];
    ImGui::PushID((int)i);
    BeginCard("##s");
    ImGui::PushFont(F.bold); ImGui::TextUnformatted(s.value("name", "").c_str()); ImGui::PopFont();
    Muted(s.value("kind", ""));
    std::string v = s.value("value", ""), id = s.value("id", ""), name = s.value("name", "");
    if (Button("Copy", true)) { copyToClipboard(v); A.toast("Copied to clipboard"); }
    ImGui::SameLine(); if (Button("Reveal")) A.info(name, v);
    ImGui::SameLine(); if (Button("Edit")) editSecret(s, st);
    ImGui::SameLine();
    if (Button("Delete", false, true)) A.confirm("Delete secret", "Delete " + name + "?", "Delete", true, [=] { vault::removeSecret(id); st->reload(); });
    EndCard();
    ImGui::PopID();
  }
}

void App::accountsPage() {
  auto st = std::make_shared<AcctState>();
  st->reload();
  reloadKeys(st);
  modal("Accounts, keys & tokens", [=] {
    if (Pill("Accounts", st->tab == 0)) st->tab = 0;
    ImGui::SameLine(); if (Pill("SSH keys", st->tab == 1)) st->tab = 1;
    ImGui::SameLine(); if (Pill("Tokens & secrets", st->tab == 2)) st->tab = 2;
    ImGui::Spacing();
    ImGui::BeginChild("##ap", ImVec2(S(780), S(520)));
    if (st->tab == 0) drawAccountsTab(*this, st);
    else if (st->tab == 1) drawKeysTab(*this, st);
    else drawSecretsTab(*this, st);
    ImGui::EndChild();
    ImGui::Spacing();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - S(110));
    return !Button("Done", true, false, ImVec2(S(110), 0)) && !ImGui::IsKeyPressed(ImGuiKey_Escape);
  }, S(830));
}

// ---------------- settings ----------------
static const char *ACCENTS[] = { "#d97757", "#c96442", "#bd5d3a", "#5b8def", "#8a6fd1", "#4f9d69", "#c58a2c", "#3aa7a3" };

void App::settingsPage() {
  struct St { int theme = 0; std::string name, email, editor, cloneDir; bool rebase = false, chunked = false, winSsh = true, sshOk = false; int chunkN = 50; float accent[3]; };
  auto s = std::make_shared<St>();
  std::string t = settings.value("theme", "system");
  s->theme = t == "light" ? 1 : t == "dark" ? 2 : 0;
  s->editor = settings.value("editor", "code"); s->cloneDir = defaultCloneDir();
  s->rebase = settings.value("pullRebase", false); s->chunked = chunkOn(); s->chunkN = chunkSize();
  s->winSsh = settings.value("globalSsh", true); s->sshOk = ssh::available();
  ImVec4 ac = hexColor(settings.value("uiAccent", "#d97757")); s->accent[0] = ac.x; s->accent[1] = ac.y; s->accent[2] = ac.z;
  bg([=] { std::string n = git::config("", "user.name"), e = git::config("", "user.email"); ui([=] { s->name = n; s->email = e; }); });
  modal("Settings", [=] {
    Heading("Appearance");
    static const char *themes[] = { "System", "Light", "Dark" };
    Label("Theme"); ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##th", &s->theme, themes, 3)) saveSettings({ { "theme", s->theme == 1 ? "light" : s->theme == 2 ? "dark" : "system" } });
    Label("Accent color");
    for (int i = 0; i < IM_ARRAYSIZE(ACCENTS); i++) {
      ImGui::PushID(i);
      ImVec4 c = hexColor(ACCENTS[i]);
      bool sel = toHex(hexColor(settings.value("uiAccent", "#d97757"))) == ACCENTS[i];
      ImVec2 p = ImGui::GetCursorScreenPos(); float d = S(26);
      if (ImGui::InvisibleButton("##sw", ImVec2(d, d))) { saveSettings({ { "uiAccent", ACCENTS[i] } }); s->accent[0] = c.x; s->accent[1] = c.y; s->accent[2] = c.z; }
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + d / 2, p.y + d / 2), d / 2 - S(2), col(c));
      if (sel) ImGui::GetWindowDrawList()->AddCircle(ImVec2(p.x + d / 2, p.y + d / 2), d / 2, col(C.text), 24, S(2));
      ImGui::SameLine();
      ImGui::PopID();
    }
    if (ImGui::ColorEdit3("##custom", s->accent, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
      saveSettings({ { "uiAccent", toHex(ImVec4(s->accent[0], s->accent[1], s->accent[2], 1)) } });
    ImGui::Spacing();
    Heading("Git");
    Field("Name", "##gn", &s->name);
    Field("Email", "##ge", &s->email);
    ImGui::Checkbox("Pull with rebase by default", &s->rebase);
#ifdef _WIN32
    std::string sshLabel = std::string("Set Windows OpenSSH as git's global core.sshCommand") + (s->sshOk ? "" : " (not installed!)");
    ImGui::Checkbox(sshLabel.c_str(), &s->winSsh);
#endif
    ImGui::Spacing();
    Heading("Slow / outdated connections");
    ImGui::Checkbox("Pull and push in chunks, with automatic retries", &s->chunked);
    Label("Commits per chunk"); ImGui::SetNextItemWidth(S(120)); ImGui::InputInt("##cn", &s->chunkN);
    ImGui::Spacing();
    Heading("Integrations");
    Field("Editor command", "##ed", &s->editor, "code");
    FolderField("Default clone folder", "##cd", &s->cloneDir);
    int a = Actions("Save");
    if (a < 0) return false;
    if (a > 0) {
      std::string n = trim(s->name), e = trim(s->email);
#ifdef _WIN32
      bool win = s->winSsh && s->sshOk;
#else
      bool win = false;
#endif
      bg([=] {
        try {
          if (!n.empty()) git::setConfig("", "user.name", n);
          if (!e.empty()) git::setConfig("", "user.email", e);
          if (win) git::setConfig("", "core.sshCommand", "C:/Windows/System32/OpenSSH/ssh.exe");  // Windows-only option; never set on Linux
        } catch (std::exception &x) { std::string m = x.what(); ui([=] { app.toast(m, true); }); }
      });
      saveSettings({ { "editor", s->editor }, { "cloneDir", s->cloneDir }, { "pullRebase", s->rebase }, { "chunked", s->chunked },
                     { "chunkSize", std::max(1, s->chunkN) }, { "gitName", n }, { "gitEmail", e }, { "globalSsh", s->winSsh } });
      return false;
    }
    return true;
  }, S(560));
}
