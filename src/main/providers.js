// REST clients for GitHub and GitLab-compatible hosts (gitlab.com, gitgud.io, self-hosted).
const vault = require('./vault');

function parseRemote(url) {
  let m = url.match(/^[a-z+]+:\/\/(?:[^@/]+@)?([^/:]+)(?::\d+)?\/(.+?)(?:\.git)?\/?$/i)
       || url.match(/^(?:[^@]+@)?([^:]+):(.+?)(?:\.git)?$/);
  if (!m) return null;
  const full = m[2].replace(/^\/+/, '');
  const parts = full.split('/');
  return { host: m[1].toLowerCase(), fullPath: full, namespace: parts.slice(0, -1).join('/'), name: parts[parts.length - 1] };
}

async function req(acct, method, url, body) {
  const headers = { Accept: 'application/json', 'User-Agent': 'KillerGit' };
  if (body) headers['Content-Type'] = 'application/json';
  if (acct.token) {
    if (acct.kind === 'github') headers.Authorization = `Bearer ${acct.token}`;
    else headers['PRIVATE-TOKEN'] = acct.token;
  }
  const res = await fetch(acct.api + url, { method, headers, body: body && JSON.stringify(body) });
  const text = await res.text();
  const json = text ? JSON.parse(text) : null;
  if (!res.ok) throw new Error(`${res.status}: ${(json && (json.message || json.error || JSON.stringify(json))) || res.statusText}`);
  return json;
}

const enc = encodeURIComponent;

const github = {
  me: a => req(a, 'GET', '/user').then(u => ({ username: u.login, name: u.name, avatar: u.avatar_url })),
  repos: a => req(a, 'GET', '/user/repos?per_page=100&sort=updated').then(rs => rs.map(r => ({
    name: r.name, fullPath: r.full_name, namespace: r.owner.login, private: r.private, updated: Date.parse(r.pushed_at || r.updated_at),
    sshUrl: r.ssh_url, httpsUrl: r.clone_url, description: r.description }))),
  namespaces: async a => {
    const me = await github.me(a);
    const orgs = await req(a, 'GET', '/user/orgs').catch(() => []);
    return [{ id: '', path: me.username, kind: 'user' }, ...orgs.map(o => ({ id: o.login, path: o.login, kind: 'org' }))];
  },
  createRepo: (a, { name, description, isPrivate, namespace }) =>
    req(a, 'POST', namespace ? `/orgs/${enc(namespace)}/repos` : '/user/repos', { name, description, private: isPrivate })
      .then(r => ({ sshUrl: r.ssh_url, httpsUrl: r.clone_url, web: r.html_url })),
  issues: (a, p, state = 'open') => req(a, 'GET', `/repos/${p}/issues?state=${state}&per_page=100`).then(is =>
    is.filter(i => !i.pull_request).map(i => ({ id: i.number, title: i.title, state: i.state, author: i.user.login,
      created: Date.parse(i.created_at), comments: i.comments, labels: i.labels.map(l => l.name), body: i.body || '', web: i.html_url }))),
  comments: (a, p, id) => req(a, 'GET', `/repos/${p}/issues/${id}/comments?per_page=100`).then(cs =>
    cs.map(c => ({ author: c.user.login, body: c.body, created: Date.parse(c.created_at) }))),
  createIssue: (a, p, title, body) => req(a, 'POST', `/repos/${p}/issues`, { title, body }),
  comment: (a, p, id, body) => req(a, 'POST', `/repos/${p}/issues/${id}/comments`, { body }),
  setIssueState: (a, p, id, open) => req(a, 'PATCH', `/repos/${p}/issues/${id}`, { state: open ? 'open' : 'closed' }),
  addSshKey: (a, title, key) => req(a, 'POST', '/user/keys', { title, key }),
};

const gitlab = {
  me: a => req(a, 'GET', '/user').then(u => ({ username: u.username, name: u.name, avatar: u.avatar_url })),
  repos: a => req(a, 'GET', '/projects?membership=true&per_page=100&order_by=last_activity_at').then(rs => rs.map(r => ({
    name: r.path, fullPath: r.path_with_namespace, namespace: r.namespace.full_path, private: r.visibility !== 'public',
    updated: Date.parse(r.last_activity_at), sshUrl: r.ssh_url_to_repo, httpsUrl: r.http_url_to_repo, description: r.description }))),
  namespaces: a => req(a, 'GET', '/namespaces?per_page=100').then(ns => ns.map(n => ({ id: n.id, path: n.full_path, kind: n.kind }))),
  createRepo: (a, { name, description, isPrivate, namespace }) =>
    req(a, 'POST', '/projects', { name, path: name, description, visibility: isPrivate ? 'private' : 'public', ...(namespace ? { namespace_id: namespace } : {}) })
      .then(r => ({ sshUrl: r.ssh_url_to_repo, httpsUrl: r.http_url_to_repo, web: r.web_url })),
  issues: (a, p, state = 'open') => req(a, 'GET', `/projects/${enc(p)}/issues?state=${state === 'open' ? 'opened' : state}&per_page=100`).then(is =>
    is.map(i => ({ id: i.iid, title: i.title, state: i.state === 'opened' ? 'open' : 'closed', author: i.author.username,
      created: Date.parse(i.created_at), comments: i.user_notes_count, labels: i.labels, body: i.description || '', web: i.web_url }))),
  comments: (a, p, id) => req(a, 'GET', `/projects/${enc(p)}/issues/${id}/notes?per_page=100&sort=asc`).then(cs =>
    cs.filter(c => !c.system).map(c => ({ author: c.author.username, body: c.body, created: Date.parse(c.created_at) }))),
  createIssue: (a, p, title, body) => req(a, 'POST', `/projects/${enc(p)}/issues`, { title, description: body }),
  comment: (a, p, id, body) => req(a, 'POST', `/projects/${enc(p)}/issues/${id}/notes`, { body }),
  setIssueState: (a, p, id, open) => req(a, 'PUT', `/projects/${enc(p)}/issues/${id}`, { state_event: open ? 'reopen' : 'close' }),
  addSshKey: (a, title, key) => req(a, 'POST', '/user/keys', { title, key }),
};

const api = a => (a.kind === 'github' ? github : gitlab);

function forRemote(url) {
  const r = parseRemote(url);
  if (!r) throw new Error('Unrecognised remote URL');
  const acct = vault.accountForHost(r.host);
  if (!acct) throw new Error(`No account configured for ${r.host}. Add one in Accounts.`);
  return { acct, path: r.fullPath };
}

module.exports = {
  parseRemote,
  me: id => api(vault.account(id)).me(vault.account(id)),
  repos: id => api(vault.account(id)).repos(vault.account(id)),
  namespaces: id => api(vault.account(id)).namespaces(vault.account(id)),
  createRepo: (id, opts) => api(vault.account(id)).createRepo(vault.account(id), opts),
  addSshKey: (id, title, key) => api(vault.account(id)).addSshKey(vault.account(id), title, key),
  issues: (url, state) => { const { acct, path } = forRemote(url); return api(acct).issues(acct, path, state); },
  comments: (url, n) => { const { acct, path } = forRemote(url); return api(acct).comments(acct, path, n); },
  createIssue: (url, t, b) => { const { acct, path } = forRemote(url); return api(acct).createIssue(acct, path, t, b); },
  comment: (url, n, b) => { const { acct, path } = forRemote(url); return api(acct).comment(acct, path, n, b); },
  setIssueState: (url, n, open) => { const { acct, path } = forRemote(url); return api(acct).setIssueState(acct, path, n, open); },
};
