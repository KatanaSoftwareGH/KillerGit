// Linux / POSIX implementation of the process / file / shell layer.
#include "util.h"
#include <imgui.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
void ui(std::function<void()> fn);

bool isAbsPath(const std::string &p) { return !p.empty() && p[0] == '/'; }
void localTime(std::time_t t, std::tm &out) { localtime_r(&t, &out); }
long long timegm_portable(std::tm *tm) { return (long long)timegm(tm); }
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------- processes ----------
static std::string which(const std::string &exe) {
  if (exe.find('/') != std::string::npos) return exe;
  const char *path = getenv("PATH");
  for (auto &d : split(path ? path : "/usr/local/bin:/usr/bin:/bin", ':')) {
    std::string p = d + "/" + exe;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  return "";
}

ProcResult runProcess(const std::string &exe, const Strings &args, const ProcOptions &opt) {
  static bool ignoredPipe = (signal(SIGPIPE, SIG_IGN), true); (void)ignoredPipe;
  std::string path = which(exe);
  if (path.empty()) throw std::runtime_error("Could not start " + exe + " (not found in PATH)");
  int in[2], out[2], err[2];
  if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(err, O_CLOEXEC)) throw std::runtime_error("pipe failed");

  std::vector<std::string> envs;
  for (char **e = environ; *e; e++) {
    std::string kv = *e; size_t eq = kv.find('=');
    if (eq != std::string::npos && opt.env.count(kv.substr(0, eq))) continue;
    envs.push_back(kv);
  }
  for (auto &[k, v] : opt.env) envs.push_back(k + "=" + v);
  std::vector<char *> argv{ (char *)path.c_str() }, envp;
  for (auto &a : args) argv.push_back((char *)a.c_str());
  argv.push_back(nullptr);
  for (auto &e : envs) envp.push_back((char *)e.c_str());
  envp.push_back(nullptr);

  pid_t pid = fork();
  if (pid == 0) {
    dup2(in[0], 0); dup2(out[1], 1); dup2(err[1], 2);
    setsid();  // no controlling terminal: ssh/git can never prompt on a tty
    if (!opt.cwd.empty() && chdir(opt.cwd.c_str()) != 0) _exit(127);
    execve(path.c_str(), argv.data(), envp.data());
    _exit(127);
  }
  close(in[0]); close(out[1]); close(err[1]);
  if (pid < 0) { close(in[1]); close(out[0]); close(err[0]); throw std::runtime_error("Could not start " + exe); }

  std::thread writer([&] {
    size_t off = 0;
    while (off < opt.input.size()) { ssize_t n = write(in[1], opt.input.data() + off, opt.input.size() - off); if (n <= 0) break; off += n; }
    close(in[1]);
  });
  ProcResult r;
  pollfd fds[2] = { { out[0], POLLIN, 0 }, { err[0], POLLIN, 0 } };
  int open = 2; char buf[65536];
  while (open > 0) {
    if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
    for (int i = 0; i < 2; i++) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t n = read(fds[i].fd, buf, sizeof buf);
      if (n <= 0) { close(fds[i].fd); fds[i].fd = -1; open--; continue; }
      if (i == 0) r.out.append(buf, n);
      else { std::string chunk(buf, n); r.err += chunk; if (opt.onStderr) opt.onStderr(chunk); }
    }
  }
  writer.join();
  int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return r;
}

// Double-fork so the child is reparented to init and never becomes a zombie.
static void spawnDetached(const Strings &argv, const std::string &cwd) {
  pid_t pid = fork();
  if (pid == 0) {
    if (fork() == 0) {
      setsid();
      int devnull = ::open("/dev/null", O_RDWR); dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
      if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
      std::vector<char *> a; for (auto &s : argv) a.push_back((char *)s.c_str()); a.push_back(nullptr);
      execvp(a[0], a.data());
      _exit(127);
    }
    _exit(0);
  }
  if (pid > 0) waitpid(pid, nullptr, 0);
}
void launchDetached(const std::string &cmdline, const std::string &cwd) { spawnDetached({ "/bin/sh", "-c", cmdline }, cwd); }

// ---------- files ----------
bool fileExists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && !S_ISDIR(st.st_mode); }
bool dirExists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
std::string readFile(const std::string &p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("Cannot read " + p);
  std::string s; char buf[65536]; ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) s.append(buf, n);
  close(fd); return s;
}
static void writeAll(int fd, const std::string &d) {
  size_t off = 0; while (off < d.size()) { ssize_t n = write(fd, d.data() + off, d.size() - off); if (n <= 0) break; off += n; }
}
void writeFile(const std::string &p, const std::string &d) {
  std::string tmp = p + ".tmp";
  // Keep the permissions of an existing file (e.g. 0600 SSH keys / vault); new files are user-only.
  struct stat st; mode_t mode = stat(p.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0600;
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) throw std::runtime_error("Cannot write " + p);
  fchmod(fd, mode); writeAll(fd, d); fsync(fd); close(fd);
  if (rename(tmp.c_str(), p.c_str()) != 0) throw std::runtime_error("Cannot write " + p);
}
void appendFile(const std::string &p, const std::string &d) {
  int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) throw std::runtime_error("Cannot write " + p);
  writeAll(fd, d); close(fd);
}
void makeDirs(const std::string &p) {
  std::string cur;
  for (auto &part : split(p, '/')) {
    cur += "/" + part;
    mkdir(cur.c_str(), 0755);
  }
}
Strings listDir(const std::string &p) {
  Strings r; DIR *d = opendir(p.c_str()); if (!d) return r;
  while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") r.push_back(n); }
  closedir(d); return r;
}
std::string homeDir() {
  const char *h = getenv("HOME");
  if (h && *h) return h;
  passwd *pw = getpwuid(getuid()); return pw ? pw->pw_dir : "/tmp";
}
std::string roamingDir() {  // XDG config home
  const char *x = getenv("XDG_CONFIG_HOME");
  return x && *x == '/' ? x : homeDir() + "/.config";
}
std::string appDataDir() {
  std::string d = roamingDir() + "/KillerGit";
  if (!dirExists(d)) { makeDirs(d); chmod(d.c_str(), 0700); }
  return d;
}
std::string documentsDir() {
  try {
    ProcResult r = runProcess("xdg-user-dir", { "DOCUMENTS" });
    std::string d = trim(r.out);
    if (r.code == 0 && dirExists(d) && d != homeDir()) return d;
  } catch (...) {}
  std::string d = homeDir() + "/Documents";
  return dirExists(d) ? d : homeDir();
}

// ---------- shell ----------
void openPath(const std::string &p) { spawnDetached({ "xdg-open", p }, ""); }
// org.freedesktop.FileManager1 highlights the file in Nautilus/Dolphin/Nemo/Thunar…; otherwise open its folder.
void showInExplorer(const std::string &p) {
  std::string uri = "file://";
  for (unsigned char c : p) { if (isalnum(c) || strchr("/-_.~", c)) uri += (char)c; else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); uri += b; } }
  std::thread([p, uri] {
    try {
      ProcResult r = runProcess("gdbus", { "call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1",
                                           "--method", "org.freedesktop.FileManager1.ShowItems", "['" + uri + "']", "" });
      if (r.code == 0) return;
    } catch (...) {}
    openPath(dirExists(p) ? p : dirName(p));
  }).detach();
}
void openUrl(const std::string &u) { if (startsWith(u, "http://") || startsWith(u, "https://")) openPath(u); }
// Goes through ImGui → GLFW, which speaks both the Wayland and X11 clipboard protocols. Must run on the UI thread.
void copyToClipboard(const std::string &s) { ui([s] { ImGui::SetClipboardText(s.c_str()); }); }

void openTerminal(const std::string &dir) {
  const char *t = getenv("TERMINAL");
  Strings candidates;
  if (t && *t) candidates.push_back(t);
  for (const char *c : { "x-terminal-emulator", "kgx", "gnome-terminal", "konsole", "ptyxis", "xfce4-terminal", "kitty", "alacritty", "foot", "wezterm", "tilix", "xterm" })
    candidates.push_back(c);
  for (auto &c : candidates)
    if (!which(c).empty()) { spawnDetached({ c }, dir); return; }  // every terminal starts in its working directory
}
void openEditor(const std::string &editor, const std::string &dir) { spawnDetached({ "/bin/sh", "-c", editor + " \"$0\"", dir }, dir); }

// zenity (GTK) or kdialog (KDE); both work under Wayland and X11.
static std::string pick(const std::string &title, bool folder) {
  try {
    if (!which("zenity").empty()) {
      Strings a = { "--file-selection", "--title=" + (title.empty() ? std::string("KillerGit") : title) };
      if (folder) a.push_back("--directory");
      ProcResult r = runProcess("zenity", a);
      if (r.code == 0) return trim(r.out);
      return "";
    }
    if (!which("kdialog").empty()) {
      ProcResult r = runProcess("kdialog", { folder ? "--getexistingdirectory" : "--getopenfilename", homeDir(), "--title", title });
      if (r.code == 0) return trim(r.out);
    }
  } catch (...) {}
  return "";
}
std::string pickFolder(const std::string &t) { return pick(t, true); }
std::string pickFile(const std::string &t) { return pick(t, false); }
