#pragma once
#include "util.h"

// Thin wrapper around the git CLI. Every call is a child process; nothing is cached here.
// Network calls get per-host credentials from the vault (HTTPS token header / SSH key).
namespace git {

struct FileStatus { std::string path, orig; char index = ' ', work = ' '; bool staged = false, unstaged = false; };
struct Status { std::string branch, upstream; int ahead = 0, behind = 0; std::vector<FileStatus> files; };
struct Commit { std::string hash, author, email, subject; Strings parents, refs; long long time = 0; };
struct CommitDetail { std::string hash, author, body; long long time = 0; std::vector<std::pair<char, std::string>> files; };
struct Branch { std::string ref, name, upstream, track, type; long long date = 0; bool current = false; };
struct Remote { std::string name, fetch, push; };
struct Stash { std::string ref, msg; long long time = 0; };
struct PushOpts { bool force = false, setUpstream = false, tags = false; };

std::string hostOf(const std::string &url);
// Runs git; throws with stderr on non-zero exit unless allowFail.
std::string run(const std::string &cwd, const Strings &args, const std::string &url = "", const Progress &progress = nullptr,
                bool allowFail = false, const std::string &input = "");

Status status(const std::string &cwd, const std::string &untracked = "all");
std::string diff(const std::string &cwd, const std::string &file, bool staged, bool untracked);
std::vector<Commit> log(const std::string &cwd, int skip = 0, int limit = 400, bool all = true);
CommitDetail show(const std::string &cwd, const std::string &hash);
std::string showFile(const std::string &cwd, const std::string &hash, const std::string &file);
std::vector<Branch> branches(const std::string &cwd);
std::vector<Remote> remotes(const std::string &cwd);
std::string remoteUrl(const std::string &cwd, const std::string &remote);
bool isRepo(const std::string &cwd);
std::string topLevel(const std::string &cwd);
long long lastActivity(const std::string &cwd);
std::string conflictState(const std::string &cwd);  // "merge" / "rebase" / "cherry-pick" / ""
std::vector<Stash> stashList(const std::string &cwd);

void init(const std::string &cwd, const std::string &branch = "main");
void clone(const std::string &url, const std::string &dest, const Progress &p, int depth = 0);
void stage(const std::string &cwd, const Strings &files);
void stageAll(const std::string &cwd);
void unstage(const std::string &cwd, const Strings &files);
void applyPatch(const std::string &cwd, const std::string &patch, bool cached, bool reverse);
void discard(const std::string &cwd, const Strings &tracked, const Strings &untracked);
void commit(const std::string &cwd, const std::string &message, bool amend = false);
void commitChunked(const std::string &cwd, const std::string &message, int size, const Progress &p);
void undoCommit(const std::string &cwd);
Strings unpushed(const std::string &cwd);  // hashes reachable from HEAD but from no remote branch
void checkout(const std::string &cwd, const std::string &name);
void checkoutRemote(const std::string &cwd, const std::string &remoteRef, const std::string &local);
void createBranch(const std::string &cwd, const std::string &name, const std::string &from = "");
void renameBranch(const std::string &cwd, const std::string &o, const std::string &n);
void deleteBranch(const std::string &cwd, const std::string &name, bool force);
void deleteRemoteBranch(const std::string &cwd, const std::string &remote, const std::string &name);
void merge(const std::string &cwd, const std::string &name, bool squash = false);
void rebase(const std::string &cwd, const std::string &onto);
void abort(const std::string &cwd);
void continueOp(const std::string &cwd, const std::string &kind);
void cherryPick(const std::string &cwd, const std::string &hash);
void revert(const std::string &cwd, const std::string &hash);
void resetTo(const std::string &cwd, const std::string &hash, const std::string &mode);
void tag(const std::string &cwd, const std::string &name, const std::string &hash);
void deleteTag(const std::string &cwd, const std::string &name);
void stash(const std::string &cwd, const std::string &message = "");
void stashPop(const std::string &cwd, const std::string &ref);
void stashDrop(const std::string &cwd, const std::string &ref);
void addRemote(const std::string &cwd, const std::string &name, const std::string &url);
void removeRemote(const std::string &cwd, const std::string &name);
void setRemoteUrl(const std::string &cwd, const std::string &name, const std::string &url);
void fetch(const std::string &cwd, const std::string &remote, const Progress &p);
void fetchAll(const std::string &cwd, const Progress &p);
void pull(const std::string &cwd, const std::string &remote, const std::string &branch, bool rebase, const Progress &p);
void push(const std::string &cwd, const std::string &remote, const std::string &branch, const PushOpts &o, const Progress &p);
void pushChunked(const std::string &cwd, const std::string &remote, const std::string &branch, int size, const Progress &p);
void pullChunked(const std::string &cwd, const std::string &remote, const std::string &branch, int size, const Progress &p);
void unshallow(const std::string &cwd, const std::string &remote, const Progress &p);
std::string config(const std::string &cwd, const std::string &key);   // cwd empty = --global
void setConfig(const std::string &cwd, const std::string &key, const std::string &val);

}  // namespace git
