#pragma once
#include "util.h"

// REST clients for GitHub and GitLab-compatible hosts (gitlab.com, gitgud.io, self-hosted), over WinHTTP.
namespace providers {

struct Parsed { std::string host, fullPath, ns, name; };
bool parseRemote(const std::string &url, Parsed &out);

struct Me { std::string username, name; };
struct HostRepo { std::string name, fullPath, ns, sshUrl, httpsUrl, description; bool isPrivate = false; long long updated = 0; };
struct Namespace { std::string id, path, kind; };
struct Created { std::string sshUrl, httpsUrl, web; };
struct Issue { long long id = 0; std::string title, state, author, body, web; long long created = 0; int comments = 0; Strings labels; };
struct Comment { std::string author, body; long long created = 0; };

Me me(const std::string &accountId);
std::vector<HostRepo> repos(const std::string &accountId);
std::vector<Namespace> namespaces(const std::string &accountId);
Created createRepo(const std::string &accountId, const std::string &name, const std::string &description, bool isPrivate, const std::string &ns);
void addSshKey(const std::string &accountId, const std::string &title, const std::string &key);

// Issue calls resolve the account from the remote URL's host.
std::vector<Issue> issues(const std::string &remoteUrl, const std::string &state);
std::vector<Comment> comments(const std::string &remoteUrl, long long id);
void createIssue(const std::string &remoteUrl, const std::string &title, const std::string &body);
void comment(const std::string &remoteUrl, long long id, const std::string &body);
void setIssueState(const std::string &remoteUrl, long long id, bool open);

}  // namespace providers
