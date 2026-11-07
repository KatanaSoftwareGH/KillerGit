// KillerGit renderer. Plain DOM, no framework, so startup stays fast.
const call = (ns, fn, ...a) => window.kg.call(ns, fn, ...a);
const G = new Proxy({}, { get: (_, fn) => (...a) => call('git', fn, ...a) });
const P = '__progress__';

const S = {
  settings: {}, repos: [], repo: null, status: null, tab: 'changes', remotes: [],
  selFile: null, history: null, selCommit: null,
  loading: null, reposLoading: false,
};

// Git writes progress to stderr as "Phase:  42% (123/456), 1.2 MiB | 300 KiB/s", redrawn with \r.
const Progress = {
  label: '', chunk: '',
  start(label) {
    this.label = label; this.chunk = '';
    $('#progress').hidden = false; $('#progress').classList.add('indet');
    $('#progress-pct').textContent = '';
    status(label + '…');
  },
  update(text) {
    logBuf += text;
    const line = text.split(/[\r\n]/).map(s => s.trim()).filter(Boolean).pop();
    if (!line) return;
    const chunk = line.match(/(?:chunk|part|round) (\d+)(?:\/(\d+))?/i);
    if (chunk) this.chunk = chunk[2] ? `chunk ${chunk[1]}/${chunk[2]}` : `round ${chunk[1]}`;
    const m = line.match(/^(?:remote:\s*)?([A-Za-z ]+?):\s+(\d+)% \(([\d/]+)\)(?:,\s*([^|]+?)(?:\s*\|\s*(.+?))?)?(?:,\s*done\.?)?$/);
    const prefix = `${this.label}${this.chunk ? ` (${this.chunk})` : ''}`;
    if (m) {
      const [, phase, pct, count, rawSize, speed] = m;
      const size = rawSize && !/^done/.test(rawSize) ? rawSize : '';
      $('#progress').classList.remove('indet');
      $('#progress-fill').style.width = pct + '%';
      $('#progress-pct').textContent = pct + '%';
      status(`${prefix} — ${phase} ${count}${size ? ' · ' + size.trim() : ''}${speed ? ' · ' + speed.trim() : ''}`);
    } else {
      status(`${prefix} — ${line.replace(/^remote:\s*/, '')}`);
    }
  },
  end() {
    $('#progress').hidden = true;
    $('#progress-fill').style.width = '0';
    $('#progress-pct').textContent = '';
  },
};
window.kg.onProgress(t => Progress.update(t));

const ACCENTS = ['#2f81f7', '#8250df', '#db61a2', '#cf222e', '#e16f24', '#d29922', '#1a7f37', '#0f9b8e'];

/* ---------- theme ---------- */
function applyTheme() {
  const t = S.settings.theme || 'system';
  const dark = t === 'dark' || (t === 'system' && matchMedia('(prefers-color-scheme: dark)').matches);
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  document.documentElement.style.setProperty('--accent', S.settings.accent || ACCENTS[0]);
}
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', applyTheme);

async function saveSettings(patch) {
  S.settings = await call('vault', 'saveSettings', patch);
  applyTheme();
}

/* ---------- busy wrapper ---------- */
let busy = false;
async function task(label, fn, { refresh = true } = {}) {
  if (busy) return toast('Another operation is running');
  busy = true;
  Progress.start(label);
  $('#status-text').classList.add('busy');
  document.body.style.cursor = 'progress';
  try {
    const r = await fn();
    status(label + ' — done');
    return r;
  } catch (e) {
    toast(`${label} failed:\n${e.message.replace(/^Error invoking remote method 'call': Error: /, '')}`, true);
  } finally {
    busy = false;
    Progress.end();
    $('#status-text').classList.remove('busy');
    document.body.style.cursor = '';
    // Refresh even on failure: earlier steps (e.g. adding a remote) may have succeeded.
    if (refresh && S.repo) await refreshRepo();
  }
}

/* ---------- repository list ---------- */
let indicatorRun = 0, reposRun = 0, treeFrame = 0;
function scheduleTree() {
  if (treeFrame) return;
  treeFrame = requestAnimationFrame(() => { treeFrame = 0; renderTree(); });
}
async function loadRepos() {
  const paths = S.settings.repos || [];
  const run = ++reposRun;
  if (treeFrame) { cancelAnimationFrame(treeFrame); treeFrame = 0; }
  S.repos = paths.map(p => ({ path: p, name: p.split(/[\\/]/).pop(), pending: true }));
  S.reposLoading = true;
  if (!paths.length) {
    S.reposLoading = false;
    return renderTree();
  }
  renderTree();

  // Fill the sidebar as summaries arrive instead of waiting for the slowest
  // repository (network drives and huge repos can take a long time).
  const queue = paths.slice();
  const worker = async () => {
    while (queue.length) {
      if (run !== reposRun) return;
      const p = queue.shift();
      let r;
      try { r = await call('repo', 'summary', p); }
      catch { r = { path: p, missing: true }; }
      if (run !== reposRun) return;
      r.name = r.name || p.split(/[\\/]/).pop();
      const i = S.repos.findIndex(x => x.path === p);
      if (i >= 0) S.repos[i] = r;
      scheduleTree();
    }
  };
  await Promise.all(Array.from({ length: Math.min(8, queue.length) }, worker));
  if (run !== reposRun) return;
  S.reposLoading = false;
  if (treeFrame) { cancelAnimationFrame(treeFrame); treeFrame = 0; }
  renderTree();
  refreshRepoIndicators();
}

// Change indicators are deliberately not part of the initial list load: `git
// status` can take a long time in big repos, and blocking the whole sidebar on
// the slowest one made every repo disappear. Check them a few at a time and
// update the dots as results arrive.
async function refreshRepoIndicators() {
  const run = ++indicatorRun;
  const queue = S.repos.filter(r => !r.missing && r.path !== S.repo).map(r => r.path);
  const worker = async () => {
    while (queue.length) {
      const p = queue.shift();
      try {
        const info = await call('repo', 'changed', p);
        if (run !== indicatorRun) return;
        const r = S.repos.find(x => x.path === p);
        if (!r || r.path === S.repo) continue;
        r.changed = info.changed || 0;
        r.ahead = info.ahead || 0;
        r.behind = info.behind || 0;
        scheduleTree();
      } catch {}
    }
  };
  const n = Math.min(3, queue.length);
  return Promise.all(Array.from({ length: n }, worker));
}

function renderTree() {
  const tree = $('#repo-tree');
  if (S.reposLoading && !S.repos.length) return throbberInto(tree, 'Loading repositories…', 'Asking git about your repos', { small: true, emoji: '📚' });
  const q = $('#repo-filter').value.toLowerCase();
  const sort = S.settings.repoSort || 'az';
  $('#repo-sort').value = sort;
  const collapsed = new Set(S.settings.collapsed || []);
  const groups = {};
  for (const r of S.repos) {
    const name = r.name || r.path.split(/[\\/]/).pop();
    if (q && !name.toLowerCase().includes(q) && !(r.namespace || '').toLowerCase().includes(q)) continue;
    const host = r.missing ? 'Missing' : r.host || 'Local only';
    const ns = r.namespace || (r.host ? '(root)' : '');
    ((groups[host] ||= {})[ns] ||= []).push({ ...r, name });
  }
  const cmp = sort === 'updated' ? (a, b) => (b.lastActivity || 0) - (a.lastActivity || 0) : (a, b) => a.name.localeCompare(b.name);
  const latest = arr => Math.max(0, ...arr.map(r => r.lastActivity || 0));
  const orderKeys = (obj, getList) => Object.keys(obj).sort(sort === 'updated'
    ? (a, b) => latest(getList(obj[b])) - latest(getList(obj[a])) : (a, b) => a.localeCompare(b));

  const toggle = key => {
    collapsed.has(key) ? collapsed.delete(key) : collapsed.add(key);
    saveSettings({ collapsed: [...collapsed] }).then(renderTree);
  };
  const item = r => h('div', {
    class: 'repo-item' + (r.path === S.repo ? ' active' : '') + (r.missing ? ' missing' : '') + (r.pending ? ' pending' : '') + (r.path === S.repo && S.loading ? ' opening' : ''),
    title: r.pending ? 'Loading repository details…' : r.path,
    onclick: () => openRepo(r.path),
    oncontextmenu: e => menu(e.clientX, e.clientY, [
      { label: 'Show in Explorer', action: () => call('sys', 'openPath', r.path) },
      { label: 'Open in terminal', action: () => call('sys', 'openTerminal', r.path) },
      { label: 'Open in editor', action: () => call('sys', 'openEditor', r.path) },
      { label: 'Copy path', action: () => call('sys', 'copy', r.path) }, '-',
      { label: 'Remove from list', action: async () => { await call('repo', 'remove', r.path); S.settings = await call('vault', 'settings'); if (S.repo === r.path) S.repo = null; loadRepos(); render(); } },
    ]),
  }, h('span', { class: 'repo-name' }, r.name),
    r.changed ? h('span', { class: 'repo-dirty', title: `${r.changed} uncommitted change${r.changed === 1 ? '' : 's'}` }) : null,
    h('span', { class: 'when' }, ago(r.lastActivity)));

  tree.innerHTML = '';
  const hosts = orderKeys(groups, o => Object.values(o).flat());
  if (!hosts.length) tree.append(h('div', { class: 'empty', style: 'padding:20px' }, 'No repositories yet.'));
  for (const host of hosts) {
    const nsObj = groups[host];
    const hostEl = h('div', { class: 'grp' + (collapsed.has(host) ? ' collapsed' : '') },
      h('div', { class: 'grp-h', onclick: () => toggle(host) }, host, h('span', { class: 'count' }, Object.values(nsObj).flat().length)));
    const body = h('div', { class: 'grp-b' });
    for (const ns of orderKeys(nsObj, l => l)) {
      const list = nsObj[ns].sort(cmp);
      if (!ns) { list.forEach(r => body.append(item(r))); continue; }
      const key = host + '/' + ns;
      body.append(h('div', { class: 'grp' + (collapsed.has(key) ? ' collapsed' : '') },
        h('div', { class: 'grp-h', onclick: () => toggle(key) }, ns, h('span', { class: 'count' }, list.length)),
        h('div', { class: 'grp-b' }, list.map(item))));
    }
    hostEl.append(body);
    tree.append(hostEl);
  }
}

async function openRepo(p) {
  if (!p) return;
  S.repo = p;
  S.selFile = null; S.history = null; S.selCommit = null;
  S.status = null; S.remotes = []; S.conflict = null;
  const name = p.split(/[\\/]/).pop();
  S.loading = { title: `Opening ${name}`, sub: 'Reading status, remotes and branches…', emoji: '📂' };
  saveSettings({ lastRepo: p });
  renderTree();
  renderToolbar();
  render();
  await refreshRepo();
}

function currentRemote() {
  const pref = (S.settings.repoRemote || {})[S.repo];
  return S.remotes.find(r => r.name === pref) ? pref : (S.remotes.find(r => r.name === 'origin') || S.remotes[0] || {}).name;
}

async function refreshRepo() {
  if (!S.repo) return render();
  const repo = S.repo;
  if (!S.status && !S.loading) {
    S.loading = { title: 'Loading repository…', sub: 'Reading status, remotes and branches…', emoji: '⏳' };
    render();
  }
  try {
    const [status, remotes, conflict] = await Promise.all([G.status(repo), G.remotes(repo), G.conflictState(repo)]);
    if (S.repo !== repo) return;
    S.status = status; S.remotes = remotes; S.conflict = conflict;
  } catch (e) {
    if (S.repo !== repo) return;
    S.status = null;
    toast(e.message, true);
  }
  if (S.repo !== repo) return;
  S.loading = null;
  const entry = S.repos.find(r => r.path === repo);
  if (entry) {
    entry.changed = S.status ? S.status.files.length : 0;
    entry.ahead = S.status ? S.status.ahead : 0;
    entry.behind = S.status ? S.status.behind : 0;
  }
  renderTree();
  renderToolbar();
  render();
}

/* ---------- toolbar ---------- */
function renderToolbar() {
  const st = S.status;
  $('#tb-repo b').textContent = S.repo ? S.repo.split(/[\\/]/).pop() : '—';
  $('#tb-branch b').textContent = st ? st.branch : '—';
  const remote = currentRemote();
  $('#tb-remote b').textContent = remote || 'none';
  let label = `Fetch ${remote || ''}`, sub = 'Fetch';
  if (st && !st.upstream && remote && st.branch) { label = `Publish branch`; sub = `to ${remote}`; }
  else if (st && st.behind) { label = `Pull ${remote} ↓${st.behind}`; sub = st.ahead ? `↑${st.ahead} to push after` : 'Behind remote'; }
  else if (st && st.ahead) { label = `Push ${remote} ↑${st.ahead}`; sub = 'Ahead of remote'; }
  $('#tb-pull-n').textContent = st && st.behind ? st.behind : '';
  $('#tb-push-n').textContent = st && st.ahead ? st.ahead : '';
  $('#tb-sync-label').textContent = label;
  $('#tb-sync-sub').textContent = sub;
}

function syncPrimary() {
  const st = S.status, r = currentRemote();
  if (!S.repo || !r) return toast('This repository has no remotes. Add one under Remotes.');
  if (!st.upstream) return doPush(r, { setUpstream: true });
  if (st.behind) return doPull(r);
  if (st.ahead) return doPush(r);
  return task(`Fetching ${r}`, () => G.fetch(S.repo, r, P));
}

const chunkOn = () => !!S.settings.chunked;
const chunkSize = () => +S.settings.chunkSize || 50;

function doPull(remote, rebase = S.settings.pullRebase) {
  const b = S.status.branch;
  if (chunkOn()) return task(`Pulling ${remote}/${b} in chunks`, () => G.pullChunked(S.repo, remote, b, chunkSize(), P));
  return task(`Pulling ${remote}/${b}`, () => G.pull(S.repo, remote, b, !!rebase, P));
}

function doPush(remote, opts = {}) {
  const b = S.status.branch;
  if (chunkOn() && !opts.force) return task(`Pushing to ${remote} in chunks`, () => G.pushChunked(S.repo, remote, b, chunkSize(), P));
  return task(`Pushing to ${remote}`, () => G.push(S.repo, remote, b, { setUpstream: opts.setUpstream || !S.status.upstream, ...opts }, P));
}

function syncMenu(el) {
  if (!S.repo) return;
  const r = currentRemote();
  const others = S.remotes.map(x => x.name);
  menuAt(el, [
    { header: `Remote: ${r || 'none'}` },
    { label: 'Fetch', action: () => task('Fetching', () => G.fetch(S.repo, r, P)) },
    { label: 'Fetch all remotes', action: () => task('Fetching all remotes', () => G.fetchAll(S.repo, P)) },
    { label: 'Pull (merge)', action: () => doPull(r, false) },
    { label: 'Pull (rebase)', action: () => doPull(r, true) },
    { label: 'Push', action: () => doPush(r) },
    { label: 'Force push (with lease)…', action: async () => (await confirmBox('Force push', `Overwrite ${r}/${S.status.branch} with your local branch?`, 'Force push', true)) && doPush(r, { force: true }) },
    { label: 'Push with tags', action: () => doPush(r, { tags: true }) }, '-',
    { header: 'Multiple remotes' },
    { label: 'Push to all remotes', action: () => task('Pushing to all remotes', async () => { for (const n of others) await G.push(S.repo, n, S.status.branch, {}, P); }) },
    ...others.filter(n => n !== r).map(n => ({ label: `Pull from ${n}`, action: () => doPull(n) })),
    ...others.filter(n => n !== r).map(n => ({ label: `Push to ${n}`, action: () => doPush(n) })), '-',
    { header: 'Slow connections' },
    { label: `${chunkOn() ? '✓ ' : ''}Chunked pull/push (${chunkSize()} commits)`, action: () => saveSettings({ chunked: !chunkOn() }).then(renderToolbar) },
    { label: 'Fetch full history (unshallow)', action: () => task('Unshallowing', () => G.unshallow(S.repo, r, P)) },
  ]);
}

function remoteMenu(el) {
  if (!S.repo) return;
  menuAt(el, [
    { header: 'Default remote for this repo' },
    ...S.remotes.map(x => ({ label: `${x.name === currentRemote() ? '✓ ' : ''}${x.name}  —  ${x.fetch}`,
      action: () => saveSettings({ repoRemote: { ...(S.settings.repoRemote || {}), [S.repo]: x.name } }).then(renderToolbar) })),
    '-', { label: 'Manage remotes…', action: () => setTab('remotes') },
  ]);
}

async function branchMenu(el) {
  if (!S.repo) return;
  const all = await G.branches(S.repo);
  const filter = h('input', { placeholder: 'Filter branches' });
  const list = h('div');
  const draw = () => {
    const q = filter.value.toLowerCase();
    list.innerHTML = '';
    const mk = (b, remote) => h('div', { class: 'mi', onclick: () => { hideMenu(); switchBranch(b, remote); } },
      (b.current ? '✓ ' : '') + b.name, h('span', { class: 'muted' }, b.track ? ' ' + b.track : ''));
    const locals = all.filter(b => b.type === 'local' && b.name.toLowerCase().includes(q)).sort((a, b) => b.date - a.date);
    const localNames = new Set(all.filter(b => b.type === 'local').map(b => b.name));
    const remotes = all.filter(b => b.type === 'remote' && b.name.toLowerCase().includes(q) && !localNames.has(b.name.replace(/^[^/]+\//, '')));
    list.append(h('div', { class: 'mh' }, 'Local'), ...locals.map(b => mk(b)));
    if (remotes.length) list.append(h('div', { class: 'mh' }, 'Remote'), ...remotes.slice(0, 50).map(b => mk(b, true)));
  };
  filter.oninput = draw;
  draw();
  menuAt(el, [{ label: '+ New branch…', action: newBranch }, '-', { el: filter }, { el: list }]);
  filter.focus();
}

async function switchBranch(b, remote) {
  if (remote) return task(`Checking out ${b.name}`, () => G.checkoutRemote(S.repo, b.name, b.name.replace(/^[^/]+\//, '')));
  if (S.status.files.length && !(await stashPrompt())) return;
  return task(`Switching to ${b.name}`, () => G.checkout(S.repo, b.name));
}

async function stashPrompt() {
  const r = await modal('Uncommitted changes', h('p', {}, 'You have uncommitted changes. What should happen to them?'), [
    { label: 'Cancel', value: null }, { label: 'Bring them along', value: 'bring' }, { label: 'Stash them', value: 'stash', primary: true }]);
  if (r === 'stash') await G.stash(S.repo, `Auto-stash on branch switch from ${S.status.branch}`);
  return r;
}

async function newBranch(from) {
  const name = await prompt('Create branch', from ? `Based on ${from.slice(0, 10)}` : `Based on ${S.status.branch}`);
  if (name) await task(`Creating ${name}`, () => G.createBranch(S.repo, name.trim().replace(/\s+/g, '-'), from));
}

/* ---------- main view ---------- */
function setTab(t) {
  S.tab = t;
  document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('active', b.dataset.tab === t));
  render();
}

function render() {
  const v = $('#view');
  v.innerHTML = '';
  if (!S.repo) return v.append(welcome());
  if (S.loading) return v.append(throbber(S.loading.title, S.loading.sub, { emoji: S.loading.emoji }));
  if (!S.status) return v.append(h('div', { class: 'empty' }, h('h2', {}, 'Repository unavailable'), h('p', {}, S.repo)));
  const views = { changes: viewChanges, history: viewHistory, branches: viewBranches, stashes: viewStashes, remotes: viewRemotes, issues: viewIssues };
  const el = views[S.tab]();
  if (S.conflict) {
    const wrap = h('div', { style: 'display:flex;flex-direction:column;flex:1;min-width:0' },
      h('div', { class: 'banner' }, h('span', { class: 'grow' }, `A ${S.conflict} is in progress. Resolve conflicts, stage the files, then continue.`),
        h('button', { onclick: () => task('Continuing', () => G.continueOp(S.repo, S.conflict)) }, 'Continue'),
        h('button', { class: 'danger', onclick: () => task('Aborting', () => G.abort(S.repo)) }, 'Abort')),
      el);
    v.append(wrap);
    wirePaneResizers();
    return;
  }
  v.append(el);
  wirePaneResizers();
}

/* ---------- resizable panes ---------- */
function installResizer(handle, target, { min = 180, max = () => innerWidth - 320, reset, onEnd } = {}) {
  let startX = 0, startW = 0, dragging = false;
  const bound = w => Math.max(min, Math.min(Math.max(min, max()), Math.round(w)));
  const move = e => {
    if (!dragging) return;
    target.style.width = bound(startW + e.clientX - startX) + 'px';
  };
  const up = () => {
    if (!dragging) return;
    dragging = false;
    document.body.classList.remove('resizing');
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', up);
    if (onEnd) onEnd(bound(target.getBoundingClientRect().width));
  };
  handle.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    e.preventDefault();
    startX = e.clientX;
    startW = target.getBoundingClientRect().width;
    dragging = true;
    document.body.classList.add('resizing');
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
  });
  handle.addEventListener('keydown', e => {
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    e.preventDefault();
    const w = bound(target.getBoundingClientRect().width + (e.key === 'ArrowRight' ? 16 : -16));
    target.style.width = w + 'px';
    if (onEnd) onEnd(w);
  });
  handle.addEventListener('dblclick', () => {
    target.style.width = reset || '';
    if (onEnd) onEnd(bound(target.getBoundingClientRect().width));
  });
}

function wirePaneResizers() {
  const defaults = { changes: '320px', history: '58%', issues: '400px' };
  document.querySelectorAll('#view .split').forEach(split => {
    if (split.dataset.resizable) return;
    const left = split.querySelector(':scope > .pane-l');
    const right = split.querySelector(':scope > .pane-r');
    if (!left || !right) return;
    split.dataset.resizable = '1';
    const key = S.tab;
    const saved = (S.settings.paneWidths || {})[key];
    const available = Math.max(240, split.getBoundingClientRect().width - 320);
    if (saved) left.style.width = Math.min(available, Math.max(240, +saved)) + 'px';
    const handle = h('div', { class: 'resizer pane-resizer', role: 'separator', 'aria-orientation': 'vertical', tabindex: '0', title: 'Drag to resize (double-click to reset)' });
    left.after(handle);
    installResizer(handle, left, {
      min: 240,
      max: () => Math.max(260, split.getBoundingClientRect().width - 320),
      reset: defaults[key] || '320px',
      onEnd: w => saveSettings({ paneWidths: { ...(S.settings.paneWidths || {}), [key]: w } }),
    });
  });
}

function welcome() {
  return h('div', { class: 'empty' }, h('h2', {}, 'Welcome to KillerGit'),
    h('p', {}, 'Clone a repository, add one from your disk, or create a new one.'),
    h('button', { class: 'primary', onclick: cloneDialog }, 'Clone repository'),
    h('button', { onclick: addLocal }, 'Add local repository'),
    h('button', { onclick: createDialog }, 'Create new repository'),
    h('button', { onclick: importGithubDesktop }, 'Import from GitHub Desktop'),
    h('p', {}, h('a', { onclick: accountsPage }, 'Set up accounts & SSH keys')));
}

/* ---------- changes ---------- */
// Builds a commit message from the file list: staged files if any, otherwise everything (what Commit would take).
function suggestMessage(files) {
  const staged = files.filter(f => f.staged);
  const set = staged.length ? staged : files;
  if (!set.length) return { summary: '', desc: '' };
  const kind = f => {
    const c = staged.length ? f.index : (f.work !== ' ' ? f.work : f.index);
    return c === '?' || c === 'A' ? 'Add' : c === 'D' ? 'Delete' : c === 'R' ? 'Rename' : 'Update';
  };
  const base = p => p.split('/').pop();
  const join = names => names.length === 1 ? names[0] : names.slice(0, -1).join(', ') + ' and ' + names[names.length - 1];
  const groups = {};
  set.forEach(f => (groups[kind(f)] = groups[kind(f)] || []).push(f));
  const verbs = Object.keys(groups);

  let summary;
  if (verbs.length === 1 && set.length <= 3) summary = `${verbs[0]} ${join(set.map(f => base(f.path)))}`;
  else if (verbs.length === 1) {
    const dirs = set.map(f => f.path.split('/').slice(0, -1));
    let common = dirs[0];
    dirs.forEach(d => { let i = 0; while (i < common.length && common[i] === d[i]) i++; common = common.slice(0, i); });
    summary = `${verbs[0]} ${set.length} files${common.length ? ' in ' + common.join('/') : ''}`;
  } else summary = verbs.map((v, i) => `${i ? v.toLowerCase() : v} ${groups[v].length === 1 ? base(groups[v][0].path) : groups[v].length + ' files'}`).join(', ');

  const past = { Add: 'Added', Delete: 'Deleted', Rename: 'Renamed', Update: 'Updated' };
  const desc = set.length < 2 ? '' : verbs.map(v => groups[v].map(f => `- ${past[v]} ${f.orig ? f.orig + ' → ' : ''}${f.path}`).join('\n')).join('\n');
  return { summary, desc };
}

function viewChanges() {
  const st = S.status;
  const files = st.files;
  const allStaged = files.length && files.every(f => f.staged && !f.unstaged);
  const list = h('div', { class: 'list' }, files.map(f => {
    const cb = h('input', { type: 'checkbox', checked: f.staged && !f.unstaged });
    cb.indeterminate = f.staged && f.unstaged;
    cb.onclick = e => { e.stopPropagation(); task('Staging', () => (cb.checked ? G.stage(S.repo, [f.path]) : G.unstage(S.repo, [f.path])), { refresh: true }); };
    const code = f.index === '?' ? '?' : f.index === 'U' || f.work === 'U' ? 'U' : (f.work !== ' ' ? f.work : f.index);
    return h('div', {
      class: 'row' + (S.selFile === f.path ? ' sel' : ''), title: f.path,
      onclick: () => { S.selFile = f.path; render(); },
      oncontextmenu: e => menu(e.clientX, e.clientY, [
        { label: 'Discard changes…', action: () => discard([f]) },
        { label: 'Ignore file', action: () => task('Ignoring', () => call('repo', 'appendIgnore', S.repo, f.path)) },
        f.path.includes('.') && { label: `Ignore all *.${f.path.split('.').pop()} files`, action: () => task('Ignoring', () => call('repo', 'appendIgnore', S.repo, '*.' + f.path.split('.').pop())) },
        '-',
        { label: 'Copy path', action: () => call('sys', 'copy', f.path) },
        { label: 'Show in Explorer', action: () => call('sys', 'showItem', S.repo + '\\' + f.path.replace(/\//g, '\\')) },
        { label: 'Open with default program', action: () => call('sys', 'openPath', S.repo + '\\' + f.path.replace(/\//g, '\\')) },
      ]),
    }, cb, h('span', { class: 'badge b-' + code }, code === '?' ? 'A' : code), h('span', { class: 'name' }, f.path));
  }));
  const allCb = h('input', { type: 'checkbox', checked: allStaged });
  allCb.onclick = () => task('Staging', () => (allCb.checked ? G.stageAll(S.repo) : G.unstage(S.repo, ['.'])));

  const suggestion = suggestMessage(files);
  const summary = h('input', { placeholder: suggestion.summary ? `${suggestion.summary} (Tab to accept)` : 'Summary (required)', value: S.draftSummary || '' });
  const desc = h('textarea', { placeholder: 'Description' }, S.draftDesc || '');
  summary.oninput = () => (S.draftSummary = summary.value);
  desc.oninput = () => (S.draftDesc = desc.value);
  summary.onkeydown = e => {
    if (e.key === 'Tab' && !e.shiftKey && !summary.value && suggestion.summary) {
      e.preventDefault();
      summary.value = S.draftSummary = suggestion.summary;
    }
  };
  const autofill = h('button', { class: 'icon', title: 'Autofill summary and description from changes', disabled: !files.length }, '✨');
  autofill.onclick = () => {
    summary.value = S.draftSummary = suggestion.summary;
    desc.value = S.draftDesc = suggestion.desc;
  };
  const amend = h('input', { type: 'checkbox' });
  const chunk = h('input', { type: 'checkbox', checked: !!S.settings.chunkCommit });
  const chunkN = h('input', { type: 'number', min: 1, value: S.settings.chunkCommitSize || 100 });
  chunk.onchange = () => saveSettings({ chunkCommit: chunk.checked });
  chunkN.onchange = () => saveSettings({ chunkCommitSize: +chunkN.value });
  const stagedCount = files.filter(f => f.staged).length;
  const commitBtn = h('button', { class: 'primary' }, `Commit ${stagedCount || 'all'} to ${st.branch}`);
  commitBtn.onclick = async () => {
    if (!summary.value.trim() && !amend.checked) return toast('Enter a commit summary');
    if (!(await ensureIdentity())) return;
    const msg = summary.value.trim() + (desc.value.trim() ? '\n\n' + desc.value.trim() : '');
    await task('Committing', async () => {
      if (!stagedCount) await G.stageAll(S.repo);
      if (chunk.checked && !amend.checked) await G.commitChunked(S.repo, msg, +chunkN.value || 100, P);
      else await G.commit(S.repo, msg || undefined, { amend: amend.checked });
      S.draftSummary = S.draftDesc = '';
    });
  };
  amend.onchange = async () => {
    if (amend.checked && !summary.value) {
      const last = await G.log(S.repo, { limit: 1, all: false });
      if (last[0]) summary.value = last[0].subject;
    }
  };

  const left = h('div', { class: 'pane-l' },
    h('div', { class: 'list-h' }, allCb, h('span', { class: 'grow' }, `${files.length} changed file${files.length === 1 ? '' : 's'}`),
      h('button', { class: 'icon', title: 'More', onclick: e => menuAt(e.currentTarget, [
        { label: 'Discard all changes…', action: () => discard(files) },
        { label: 'Stash all changes', action: () => task('Stashing', () => G.stash(S.repo)) },
        { label: 'Undo last commit', action: () => task('Undoing commit', () => G.undoCommit(S.repo)) },
        { label: 'Refresh', action: refreshRepo },
      ]) }, '⋯')),
    list,
    h('div', { class: 'commit-box' }, h('div', { class: 'summary-row' }, summary, autofill), desc,
      h('div', { class: 'opts' }, h('label', { class: 'check' }, amend, 'Amend last commit'),
        h('label', { class: 'check', title: 'Split staged files into several smaller commits' }, chunk, 'Commit in chunks of', chunkN, 'files')),
      commitBtn));
  const right = h('div', { class: 'pane-r' });
  const f = files.find(x => x.path === S.selFile) || files[0];
  if (f) { S.selFile = f.path; loadFileDiff(right, f); }
  else right.append(h('div', { class: 'empty', style: 'margin-top:20vh' }, h('h2', {}, 'No local changes'),
    st.ahead ? h('p', {}, `You have ${st.ahead} commit(s) to push.`) : h('p', {}, 'Working tree is clean.'),
    h('button', { onclick: () => call('sys', 'openEditor', S.repo) }, 'Open in editor'),
    h('button', { onclick: () => call('sys', 'openPath', S.repo) }, 'Show in Explorer')));
  return h('div', { class: 'split' }, left, right);
}

// Git refuses to commit without user.name/user.email; ask once and save globally.
async function ensureIdentity() {
  const [n, e] = await Promise.all([G.config(S.repo, 'user.name'), G.config(S.repo, 'user.email')]);
  if (n && e) return true;
  const name = h('input', { value: n || S.settings.gitName || '' });
  const email = h('input', { value: e || S.settings.gitEmail || '', placeholder: 'Use the email of your GitHub/GitLab account' });
  return !!(await modal('Who are you?', [h('p', { class: 'muted' }, 'Git records a name and email on every commit. This is saved globally and can be changed in Settings.'),
    field('Name', name), field('Email', email)], [{ label: 'Cancel' }, { label: 'Save', primary: true, action: async () => {
    if (!name.value.trim() || !email.value.trim()) throw new Error('Both name and email are required');
    await G.setConfig(null, 'user.name', name.value.trim());
    await G.setConfig(null, 'user.email', email.value.trim());
    await saveSettings({ gitName: name.value.trim(), gitEmail: email.value.trim() });
    return true;
  } }]));
}

async function loadFileDiff(el, f) {
  const untracked = f.index === '?';
  const token = (el._diffToken = (el._diffToken || 0) + 1);
  el.innerHTML = '';
  el.append(throbber('Loading diff…', f.path.split('/').pop(), { small: true, emoji: '🔍' }));
  let parts;
  try {
    parts = [];
    if (f.staged) parts.push(['Staged', await G.diff(S.repo, f.path, true), 'staged']);
    if (f.unstaged || untracked) parts.push([untracked ? 'New file' : 'Unstaged', await G.diff(S.repo, f.path, false, untracked), untracked ? 'untracked' : 'unstaged']);
  } catch (e) {
    if (token === el._diffToken) throbberInto(el, 'Could not load diff', e.message, { small: true, emoji: '💥' });
    return;
  }
  if (token !== el._diffToken) return;
  el.innerHTML = '';
  for (const [title, text, kind] of parts) el.append(renderDiff(text, `${title}: ${f.path}`, kind));
}

function renderDiff(text, title, kind) {
  const root = h('div', { class: 'diff' });
  if (title) root.append(h('div', { class: 'fh' }, title));
  if (!text.trim()) { root.append(h('div', { class: 'muted', style: 'padding:10px' }, 'No textual changes (binary or mode change).')); return root; }
  if (text.length > 2_000_000) { root.append(h('div', { class: 'muted', style: 'padding:10px' }, 'Diff too large to display.')); return root; }
  const lines = text.split('\n');
  let header = [], i = 0;
  while (i < lines.length && !lines[i].startsWith('@@')) header.push(lines[i++]);
  let oldN = 0, newN = 0;
  while (i < lines.length) {
    const l = lines[i];
    if (l.startsWith('@@')) {
      const start = i;
      let end = i + 1;
      while (end < lines.length && !lines[end].startsWith('@@') && !lines[end].startsWith('diff --git')) end++;
      const patch = [...header, ...lines.slice(start, end)].join('\n') + '\n';
      const m = l.match(/-(\d+)(?:,\d+)? \+(\d+)/);
      oldN = +m[1]; newN = +m[2];
      const btns = [];
      if (kind === 'unstaged') {
        btns.push(h('button', { onclick: () => task('Staging hunk', () => G.applyPatch(S.repo, patch, { cached: true })) }, 'Stage hunk'));
        btns.push(h('button', { class: 'danger', onclick: async () => (await confirmBox('Discard hunk', 'Discard this hunk? This cannot be undone.', 'Discard', true)) && task('Discarding hunk', () => G.applyPatch(S.repo, patch, { cached: false, reverse: true })) }, 'Discard'));
      } else if (kind === 'staged') {
        btns.push(h('button', { onclick: () => task('Unstaging hunk', () => G.applyPatch(S.repo, patch, { cached: true, reverse: true })) }, 'Unstage hunk'));
      }
      root.append(h('div', { class: 'hk' }, h('span', {}, l), h('span', {}, btns)));
      i++;
      continue;
    }
    if (l.startsWith('diff --git')) {
      header = [];
      root.append(h('div', { class: 'fh' }, l.replace(/^diff --git a\/.* b\//, '')));
      while (i < lines.length && !lines[i].startsWith('@@')) header.push(lines[i++]);
      continue;
    }
    const c = l[0];
    const cls = c === '+' ? 'add' : c === '-' ? 'del' : '';
    const no = c === '+' ? newN++ : c === '-' ? oldN++ : (oldN++, newN++);
    if (l || i < lines.length - 1) root.append(h('div', { class: 'ln ' + cls }, h('span', { class: 'no' }, c === '\\' ? '' : no), h('span', { class: 'tx' }, l)));
    i++;
  }
  return root;
}

async function discard(files) {
  if (!(await confirmBox('Discard changes', `Discard changes to ${files.length} file(s)? This cannot be undone.`, 'Discard', true))) return;
  const untracked = files.filter(f => f.index === '?').map(f => f.path);
  const tracked = files.filter(f => f.index !== '?' && f.index !== 'A').map(f => f.path);
  const added = files.filter(f => f.index === 'A').map(f => f.path);
  await task('Discarding', async () => {
    if (added.length) { await G.unstage(S.repo, added); untracked.push(...added); }
    await G.discard(S.repo, tracked, untracked);
  });
}

/* ---------- history ---------- */
function viewHistory() {
  const listEl = h('div', { class: 'list history' });
  const detail = h('div', { class: 'pane-r' });
  const filter = h('input', { placeholder: 'Filter by message, author or SHA', style: 'flex:1' });
  const allBranches = h('input', { type: 'checkbox', checked: S.settings.historyAll !== false });
  const left = h('div', { class: 'pane-l', style: 'width:58%' },
    h('div', { class: 'list-h' }, filter, h('label', { class: 'check' }, allBranches, 'All branches')), listEl);
  allBranches.onchange = () => { saveSettings({ historyAll: allBranches.checked }); S.history = null; render(); };

  const draw = () => {
    const q = filter.value.toLowerCase();
    const { commits, rows } = S.history;
    const width = Graph.widthFor(rows);
    listEl.innerHTML = '';
    const frag = document.createDocumentFragment();
    commits.forEach((c, i) => {
      if (q && !(c.subject.toLowerCase().includes(q) || c.author.toLowerCase().includes(q) || c.hash.startsWith(q))) return;
      const refs = c.refs.map(r => {
        const head = r.startsWith('HEAD -> ') || r === 'HEAD';
        const name = r.replace('HEAD -> ', '').replace('tag: ', '');
        return h('span', { class: 'ref' + (head ? ' head' : '') + (r.startsWith('tag:') ? ' tag' : '') + (name.includes('/') && !head ? ' remote' : '') }, name);
      });
      const row = h('div', { class: 'row' + (S.selCommit === c.hash ? ' sel' : '') },
        q ? h('span', { style: 'width:8px' }) : h('span', { html: Graph.svg(rows[i], width) }),
        refs, h('span', { class: 'subj' }, c.subject), h('span', { class: 'meta' }, `${c.author} · ${ago(c.time)}`));
      row.onclick = () => { S.selCommit = c.hash; listEl.querySelectorAll('.sel').forEach(x => x.classList.remove('sel')); row.classList.add('sel'); showCommit(detail, c.hash); };
      row.oncontextmenu = e => commitMenu(e, c);
      frag.append(row);
    });
    listEl.append(frag);
    if (S.history.more) listEl.append(h('div', { style: 'padding:10px;text-align:center' }, h('button', { onclick: loadMore }, 'Load more')));
  };
  const loadMore = async () => {
    throbberInto(listEl, 'Loading more commits…', `Already at ${S.history.commits.length}`, { small: true, emoji: '📜' });
    try {
      const more = await G.log(S.repo, { skip: S.history.commits.length, limit: 400, all: allBranches.checked });
      const lay = Graph.layout(more, S.history.lanes);
      S.history.commits.push(...more); S.history.rows.push(...lay.rows); S.history.lanes = lay.lanes; S.history.more = more.length === 400;
      draw();
    } catch (e) { draw(); toast(e.message, true); }
  };
  filter.oninput = draw;
  if (!S.history) listEl.append(throbber('Wrangling the commit graph…', 'Big repositories take a moment', { small: true, emoji: '🕸️' }));
  (async () => {
    if (!S.history) {
      try {
        const commits = await G.log(S.repo, { limit: 400, all: allBranches.checked });
        const lay = Graph.layout(commits);
        S.history = { commits, rows: lay.rows, lanes: lay.lanes, more: commits.length === 400 };
      } catch (e) { throbberInto(listEl, 'Could not load history', e.message, { small: true, emoji: '💥' }); return; }
    }
    draw();
    if (S.selCommit) showCommit(detail, S.selCommit);
  })();
  return h('div', { class: 'split' }, left, detail);
}

async function showCommit(el, hash) {
  throbberInto(el, 'Loading commit…', hash.slice(0, 10), { small: true, emoji: '🧾' });
  let c;
  try { c = await G.show(S.repo, hash); }
  catch (e) { throbberInto(el, 'Could not load commit', e.message, { small: true, emoji: '💥' }); toast(e.message, true); return; }
  const [subject, ...rest] = c.body.trim().split('\n');
  const diffBox = h('div');
  const loadFile = async path => {
    throbberInto(diffBox, 'Loading file diff…', path.split('/').pop(), { small: true, emoji: '📄' });
    try {
      const text = await G.showFile(S.repo, hash, path);
      diffBox.innerHTML = '';
      diffBox.append(renderDiff(text, path));
    } catch (e) { throbberInto(diffBox, 'Could not load file diff', e.message, { small: true, emoji: '💥' }); }
  };
  el.innerHTML = '';
  el.append(h('div', { class: 'detail' },
    h('h3', {}, subject), rest.join('\n').trim() && h('p', { class: 'issue-body muted' }, rest.join('\n').trim()),
    h('div', { class: 'muted' }, `${c.author} · ${new Date(c.time * 1000).toLocaleString()} · `, h('a', { onclick: () => call('sys', 'copy', c.hash) }, c.hash.slice(0, 10))),
    h('div', { style: 'margin:10px 0' }, c.files.map(f => h('div', { class: 'row', onclick: () => loadFile(f.path) },
      h('span', { class: 'badge b-' + f.status }, f.status), h('span', { class: 'name' }, f.path)))),
    diffBox));
  if (c.files[0]) await loadFile(c.files[0].path);
}

function commitMenu(e, c) {
  menu(e.clientX, e.clientY, [
    { label: 'Copy SHA', action: () => call('sys', 'copy', c.hash) },
    { label: 'Copy subject', action: () => call('sys', 'copy', c.subject) }, '-',
    { label: 'Create branch from commit…', action: () => newBranch(c.hash) },
    { label: 'Create tag…', action: async () => { const n = await prompt('Create tag', 'Tag name'); if (n) task('Tagging', () => G.tag(S.repo, n, c.hash)); } },
    { label: 'Checkout (detached)', action: () => task('Checking out', () => G.checkout(S.repo, c.hash)) }, '-',
    { label: 'Cherry-pick onto current branch', action: () => task('Cherry-picking', () => G.cherryPick(S.repo, c.hash)) },
    { label: 'Revert this commit', action: () => task('Reverting', () => G.revert(S.repo, c.hash)) },
    { label: 'Reset current branch here (soft)', action: () => task('Resetting', () => G.resetTo(S.repo, c.hash, 'soft')) },
    { label: 'Reset current branch here (mixed)', action: () => task('Resetting', () => G.resetTo(S.repo, c.hash, 'mixed')) },
    { label: 'Reset current branch here (hard)…', action: async () => (await confirmBox('Hard reset', 'All uncommitted changes will be lost.', 'Reset', true)) && task('Resetting', () => G.resetTo(S.repo, c.hash, 'hard')) },
  ]);
}

/* ---------- branches ---------- */
function viewBranches() {
  const page = h('div', { class: 'page' }, throbber('Fetching branches…', 'Local, remote and tags', { small: true, emoji: '🌿' }));
  (async () => {
    let all;
    try { all = await G.branches(S.repo); }
    catch (e) { throbberInto(page, 'Could not load branches', e.message, { small: true, emoji: '💥' }); return; }
    const sorted = list => list.sort((a, b) => b.date - a.date);
    const table = (rows, cols) => h('table', { class: 'table' }, h('tr', {}, cols.map(c => h('th', {}, c))), rows);
    const local = sorted(all.filter(b => b.type === 'local')).map(b => h('tr', {},
      h('td', {}, b.current ? h('b', {}, '✓ ' + b.name) : b.name), h('td', { class: 'muted' }, b.upstream || '—', ' ', b.track),
      h('td', { class: 'muted' }, ago(b.date)),
      h('td', {}, !b.current && h('button', { onclick: () => switchBranch(b) }, 'Checkout'), ' ',
        h('button', { onclick: e => menuAt(e.currentTarget, [
          !b.current && { label: `Merge into ${S.status.branch}`, action: () => task('Merging', () => G.merge(S.repo, b.name)) },
          !b.current && { label: `Squash-merge into ${S.status.branch}`, action: () => task('Squash merging', () => G.merge(S.repo, b.name, true)) },
          !b.current && { label: `Rebase ${S.status.branch} onto this`, action: () => task('Rebasing', () => G.rebase(S.repo, b.name)) },
          { label: 'Rename…', action: async () => { const n = await prompt('Rename branch', 'New name', b.name); if (n) task('Renaming', () => G.renameBranch(S.repo, b.name, n)); } },
          { label: 'Create branch from here…', action: () => newBranch(b.name) },
          !b.current && { label: 'Delete…', action: async () => (await confirmBox('Delete branch', `Delete ${b.name}?`, 'Delete', true)) && task('Deleting', () => G.deleteBranch(S.repo, b.name, true)) },
        ]) }, '⋯'))));
    const remote = sorted(all.filter(b => b.type === 'remote')).map(b => h('tr', {},
      h('td', {}, b.name), h('td', { class: 'muted' }, ago(b.date)),
      h('td', {}, h('button', { onclick: () => switchBranch(b, true) }, 'Checkout'), ' ',
        h('button', { onclick: () => task('Merging', () => G.merge(S.repo, b.name)) }, 'Merge'), ' ',
        h('button', { class: 'danger', onclick: async () => {
          const [r, ...n] = b.name.split('/');
          if (await confirmBox('Delete remote branch', `Delete ${b.name} on the server?`, 'Delete', true)) task('Deleting remote branch', () => G.deleteRemoteBranch(S.repo, r, n.join('/')));
        } }, 'Delete'))));
    const tags = sorted(all.filter(b => b.type === 'tag')).map(b => h('tr', {}, h('td', {}, b.name), h('td', { class: 'muted' }, ago(b.date)),
      h('td', {}, h('button', { class: 'danger', onclick: () => task('Deleting tag', () => G.deleteTag(S.repo, b.name)) }, 'Delete'))));
    page.innerHTML = '';
    page.append(
      h('div', { class: 'toolbar-row' }, h('button', { class: 'primary', onclick: () => newBranch() }, 'New branch')),
      h('section', {}, h('h3', {}, 'Local branches'), table(local, ['Name', 'Upstream', 'Updated', ''])),
      h('section', {}, h('h3', {}, 'Remote branches'), table(remote, ['Name', 'Updated', ''])),
      tags.length ? h('section', {}, h('h3', {}, 'Tags'), table(tags, ['Name', 'Date', ''])) : null);
  })();
  return page;
}

/* ---------- stashes ---------- */
function viewStashes() {
  const page = h('div', { class: 'page' }, throbber('Digging through stashes…', 'Checking for hidden work', { small: true, emoji: '🧺' }));
  (async () => {
    let list;
    try { list = await G.stashList(S.repo); }
    catch (e) { throbberInto(page, 'Could not load stashes', e.message, { small: true, emoji: '💥' }); return; }
    page.innerHTML = '';
    page.append(h('div', { class: 'toolbar-row' }, h('button', { class: 'primary', onclick: async () => {
      const m = await prompt('Stash changes', 'Message (optional)');
      if (m !== null) task('Stashing', () => G.stash(S.repo, m));
    } }, 'Stash current changes')));
    if (!list.length) page.append(h('p', { class: 'muted' }, 'No stashes.'));
    list.forEach(s => page.append(h('div', { class: 'card' }, h('div', { class: 'title' }, s.msg), h('div', { class: 'muted' }, `${s.ref} · ${ago(s.time)}`),
      h('div', { class: 'actions' },
        h('button', { onclick: () => task('Applying stash', () => G.stashPop(S.repo, s.ref)) }, 'Restore'),
        h('button', { onclick: async () => { const d = h('div'); d.append(renderDiff(await G.showFile(S.repo, s.ref, '.'), '')); modal(s.msg, d, [], { wide: true }); } }, 'View'),
        h('button', { class: 'danger', onclick: async () => (await confirmBox('Drop stash', 'Delete this stash?', 'Delete', true)) && task('Dropping', () => G.stashDrop(S.repo, s.ref)) }, 'Discard')))));
  })();
  return page;
}

/* ---------- remotes ---------- */
function viewRemotes() {
  const page = h('div', { class: 'page' },
    h('div', { class: 'toolbar-row' },
      h('button', { class: 'primary', onclick: () => publishDialog() }, 'Create repository on a host…'),
      h('button', { onclick: addRemoteDialog }, 'Add remote URL…'),
      h('button', { onclick: () => task('Fetching all', () => G.fetchAll(S.repo, P)) }, 'Fetch all')),
    h('p', { class: 'muted' }, 'A repository can have any number of remotes. Pick the default in the toolbar; push/pull to others from the sync menu.'));
  if (!S.remotes.length) page.append(h('p', {}, 'No remotes configured.'));
  for (const r of S.remotes) {
    page.append(h('div', { class: 'card' },
      h('div', { class: 'title' }, r.name, r.name === currentRemote() ? h('span', { class: 'label', style: 'margin-left:8px' }, 'default') : null),
      h('div', { class: 'mono muted' }, 'fetch: ' + r.fetch), r.push !== r.fetch && h('div', { class: 'mono muted' }, 'push: ' + r.push),
      h('div', { class: 'actions' },
        h('button', { onclick: () => saveSettings({ repoRemote: { ...(S.settings.repoRemote || {}), [S.repo]: r.name } }).then(() => { renderToolbar(); render(); }) }, 'Make default'),
        h('button', { onclick: () => task(`Fetching ${r.name}`, () => G.fetch(S.repo, r.name, P)) }, 'Fetch'),
        h('button', { onclick: () => doPull(r.name) }, 'Pull'),
        h('button', { onclick: () => doPush(r.name) }, 'Push'),
        h('button', { onclick: async () => { const u = await prompt('Edit remote URL', r.name, r.fetch); if (u) task('Updating', () => G.setRemoteUrl(S.repo, r.name, u)); } }, 'Edit URL'),
        h('button', { onclick: () => call('sys', 'copy', r.fetch) }, 'Copy URL'),
        h('button', { class: 'danger', onclick: async () => (await confirmBox('Remove remote', `Remove ${r.name}?`, 'Remove', true)) && task('Removing', () => G.removeRemote(S.repo, r.name)) }, 'Remove'))));
  }
  return page;
}

async function addRemoteDialog() {
  const name = h('input', { value: S.remotes.length ? '' : 'origin', placeholder: 'e.g. gitgud' });
  const url = h('input', { placeholder: 'git@host:namespace/repo.git or https://…' });
  await modal('Add remote', [field('Name', name), field('URL', url)], [{ label: 'Cancel' },
    { label: 'Add', primary: true, action: () => {
      const n = name.value.trim(), u = url.value.trim();
      if (!n || !u) throw new Error('Name and URL are required');
      return S.remotes.some(r => r.name === n)
        ? task(`Updating ${n} URL`, () => G.setRemoteUrl(S.repo, n, u))
        : task('Adding remote', () => G.addRemote(S.repo, n, u));
    } }]);
}

/* ---------- create / publish on remote host ---------- */
async function publishDialog({ localPath = S.repo, cloneAfter = false } = {}) {
  const accounts = await call('vault', 'listAccounts');
  if (!accounts.length) { toast('Add an account first (🔑 → Accounts).'); return accountsPage(); }
  const acct = h('select', {}, accounts.map(a => h('option', { value: a.id }, `${a.label || a.username || a.provider} — ${a.host}`)));
  const ns = h('select');
  const loadNs = async () => {
    ns.innerHTML = '<option>Loading…</option>';
    try {
      const list = await call('api', 'namespaces', acct.value);
      ns.innerHTML = '';
      list.forEach(n => ns.append(h('option', { value: n.id }, `${n.path} (${n.kind})`)));
    } catch (e) { ns.innerHTML = ''; ns.append(h('option', { value: '' }, 'Personal')); toast(e.message); }
  };
  acct.onchange = loadNs; loadNs();
  const name = h('input', { value: localPath ? localPath.split(/[\\/]/).pop() : '' });
  const desc = h('input');
  const priv = h('input', { type: 'checkbox', checked: true });
  const proto = h('select', {}, h('option', { value: 'ssh' }, 'SSH'), h('option', { value: 'https' }, 'HTTPS'));
  const remoteName = h('input', { value: S.remotes && S.remotes.some(r => r.name === 'origin') ? '' : 'origin', placeholder: 'e.g. github, gitgud' });
  const push = h('input', { type: 'checkbox', checked: true });
  const body = [field('Account', acct), field('Owner / namespace', ns), field('Name', name), field('Description', desc),
    h('label', { class: 'check' }, priv, 'Private repository'), field('Clone protocol', proto)];
  if (localPath) body.push(field('Local remote name', remoteName), h('label', { class: 'check' }, push, 'Push current branch after creating'));
  await modal(localPath ? 'Create repository on host & add as remote' : 'Create repository on host', body, [{ label: 'Cancel' }, {
    label: 'Create', primary: true, action: () => task('Creating repository', async () => {
      const r = await call('api', 'createRepo', acct.value, { name: name.value.trim(), description: desc.value, isPrivate: priv.checked, namespace: ns.value });
      const url = proto.value === 'ssh' ? r.sshUrl : r.httpsUrl;
      if (localPath) {
        const rn = remoteName.value.trim() || accounts.find(a => a.id === acct.value).provider;
        await G.addRemote(localPath, rn, url);
        if (push.checked && S.status && S.status.branch) await G.push(localPath, rn, S.status.branch, { setUpstream: true }, P);
      } else if (cloneAfter) {
        await cloneTo(url);
      }
      toast(`Created ${r.web}`);
    }),
  }]);
}

/* ---------- issues ---------- */
function viewIssues() {
  const remotes = S.remotes.filter(r => r.fetch);
  if (!remotes.length) return h('div', { class: 'empty' }, h('h2', {}, 'No remotes'), h('p', {}, 'Issues come from the hosting service of a remote.'));
  const sel = h('select', {}, remotes.map(r => h('option', { value: r.fetch, selected: r.name === currentRemote() }, `${r.name} (${r.fetch})`)));
  const state = h('select', {}, h('option', { value: 'open' }, 'Open'), h('option', { value: 'closed' }, 'Closed'), h('option', { value: 'all' }, 'All'));
  const filter = h('input', { placeholder: 'Filter issues' });
  const list = h('div', { class: 'list' });
  const detail = h('div', { class: 'pane-r' });
  let issues = [];
  const draw = () => {
    const q = filter.value.toLowerCase();
    list.innerHTML = '';
    issues.filter(i => !q || i.title.toLowerCase().includes(q) || String(i.id) === q).forEach(i => list.append(h('div', {
      class: 'row', style: 'white-space:normal;align-items:flex-start;padding:8px 10px', onclick: () => showIssue(i),
    }, h('span', { class: 'state-' + i.state }, i.state === 'open' ? '●' : '✓'),
      h('div', { style: 'flex:1' }, h('div', {}, i.title), h('div', { class: 'muted', style: 'font-size:11px' }, `#${i.id} by ${i.author} · ${ago(i.created / 1000)} · 💬 ${i.comments}`),
        i.labels.length ? h('div', {}, i.labels.map(l => h('span', { class: 'label' }, l))) : null))));
    if (!issues.length) list.append(h('div', { class: 'muted', style: 'padding:12px' }, 'No issues.'));
  };
  const load = async () => {
    const remoteLabel = sel.options[sel.selectedIndex] ? sel.options[sel.selectedIndex].text : 'remote';
    throbberInto(list, 'Summoning issues…', remoteLabel, { small: true, emoji: '🐞' });
    try { issues = await call('api', 'issues', sel.value, state.value); draw(); }
    catch (e) { list.innerHTML = ''; list.append(h('div', { style: 'padding:12px', class: 'muted' }, e.message.replace(/^.*Error: /, ''))); }
  };
  const showIssue = async i => {
    detail.innerHTML = '';
    const comments = h('div', {}, throbber('Loading comments…', '', { small: true, emoji: '💬' }));
    const reply = h('textarea', { placeholder: 'Leave a comment', rows: 4, style: 'width:100%' });
    detail.append(h('div', { class: 'detail' },
      h('h3', {}, i.title, h('span', { class: 'muted' }, ` #${i.id}`)),
      h('div', { class: 'muted' }, h('span', { class: 'state-' + i.state }, i.state), ` · opened by ${i.author} · `, h('a', { onclick: () => call('sys', 'openUrl', i.web) }, 'Open in browser')),
      h('div', { class: 'card issue-body', style: 'margin-top:10px' }, i.body || h('i', { class: 'muted' }, 'No description')),
      comments, reply,
      h('div', { class: 'toolbar-row', style: 'margin-top:8px' },
        h('button', { class: 'primary', onclick: () => task('Commenting', async () => { await call('api', 'comment', sel.value, i.id, reply.value); showIssue(i); }, { refresh: false }) }, 'Comment'),
        h('button', { onclick: () => task(i.state === 'open' ? 'Closing issue' : 'Reopening issue', async () => {
          await call('api', 'setIssueState', sel.value, i.id, i.state !== 'open'); i.state = i.state === 'open' ? 'closed' : 'open'; showIssue(i); load();
        }, { refresh: false }) }, i.state === 'open' ? 'Close issue' : 'Reopen issue'))));
    const cs = await call('api', 'comments', sel.value, i.id).catch(e => (toast(e.message), []));
    comments.innerHTML = '';
    cs.forEach(c => comments.append(h('div', { class: 'card' }, h('div', { class: 'muted' }, `${c.author} · ${ago(c.created / 1000)}`), h('div', { class: 'issue-body' }, c.body))));
  };
  const newIssue = async () => {
    const t = h('input'), b = h('textarea', { rows: 8 });
    await modal('New issue', [field('Title', t), field('Description', b)], [{ label: 'Cancel' },
      { label: 'Create', primary: true, action: () => task('Creating issue', async () => { await call('api', 'createIssue', sel.value, t.value, b.value); load(); }, { refresh: false }) }]);
  };
  sel.onchange = state.onchange = load;
  filter.oninput = draw;
  load();
  return h('div', { class: 'split' },
    h('div', { class: 'pane-l', style: 'width:400px' },
      h('div', { class: 'list-h', style: 'flex-wrap:wrap' }, sel, state, h('button', { class: 'primary', onclick: newIssue }, 'New'), filter), list),
    detail);
}

/* ---------- add / clone / create ---------- */
function addMenu(el) {
  menuAt(el, [
    { label: 'Clone repository…', action: cloneDialog },
    { label: 'Add existing local repository…', action: addLocal },
    { label: 'Create new local repository…', action: createDialog },
    { label: 'Create repository on a host (and clone)…', action: () => publishDialog({ localPath: null, cloneAfter: true }) },
    '-',
    { label: 'Import from GitHub Desktop…', action: importGithubDesktop },
  ]);
}

async function addLocal() {
  const dir = await call('sys', 'pickFolder', 'Choose a repository folder');
  if (!dir) return;
  if (!(await G.isRepo(dir))) {
    if (await confirmBox('Not a repository', 'That folder is not a Git repository. Initialize one there?', 'Initialize')) await G.init(dir);
    else return;
  }
  const top = await call('repo', 'add', dir);
  S.settings = await call('vault', 'settings');
  await loadRepos();
  openRepo(top);
}

async function importGithubDesktop() {
  const lm = loadingModal('Import from GitHub Desktop', throbber('Scanning GitHub Desktop…', 'Digging through its local database and logs', { emoji: '🕵️' }));
  let repos;
  try { repos = await call('sys', 'githubDesktopRepos'); }
  catch (e) { lm.close(); return toast(e.message, true); }
  lm.close();
  if (!repos.length) return toast('No repositories found in GitHub Desktop. Make sure GitHub Desktop has been used on this machine.');
  const existing = new Set(S.settings.repos || []);
  const allRepos = repos.slice().sort((a, b) => a.localeCompare(b));
  const newCount = allRepos.filter(p => !existing.has(p)).length;

  const filter = h('input', { placeholder: 'Filter repositories…', style: 'flex:1' });
  const selectAllCb = h('input', { type: 'checkbox', checked: newCount > 0 });
  const countLabel = h('span', { class: 'muted', style: 'font-size:12px' }, '');

  const items = allRepos.map(p => {
    const isNew = !existing.has(p);
    const cb = h('input', { type: 'checkbox', checked: isNew });
    const badge = !isNew ? h('span', { class: 'muted', style: 'font-size:11px;margin-left:auto' }, '(already added)') : null;
    const row = h('div', { class: 'row', onclick: (e) => {
      if (e.target !== cb) cb.checked = !cb.checked;
      updateCount();
    } }, cb, h('span', { class: 'name', style: 'direction:ltr;font-family:monospace;font-size:12px' }, p), badge);
    cb.onchange = () => updateCount();
    return { path: p, cb, row, isNew };
  });

  const updateCount = () => {
    const selCount = items.filter(i => i.cb.checked).length;
    countLabel.textContent = `${selCount} of ${allRepos.length} selected`;
    selectAllCb.checked = selCount === allRepos.length;
    selectAllCb.indeterminate = selCount > 0 && selCount < allRepos.length;
  };
  updateCount();

  selectAllCb.onchange = () => {
    items.forEach(i => (i.cb.checked = selectAllCb.checked));
    updateCount();
  };

  const list = h('div', { style: 'max-height:360px;overflow:auto;border:1px solid var(--border);border-radius:6px' }, items.map(i => i.row));

  filter.oninput = () => {
    const q = filter.value.toLowerCase();
    items.forEach(i => {
      i.row.style.display = !q || i.path.toLowerCase().includes(q) ? '' : 'none';
    });
  };

  const header = h('div', { class: 'list-h', style: 'padding:6px 10px;gap:8px;align-items:center' },
    h('label', { class: 'check' }, selectAllCb, countLabel),
    filter
  );

  const subtext = newCount > 0
    ? `Found ${repos.length} repository(ies) in GitHub Desktop (${newCount} new to KillerGit).`
    : `Found ${repos.length} repository(ies) in GitHub Desktop (all are already added to KillerGit).`;

  await modal('Import from GitHub Desktop', [
    h('p', { class: 'muted', style: 'margin:0 0 6px' }, subtext),
    header,
    list,
  ], [{ label: 'Cancel' }, { label: 'Import selected', primary: true, action: async (btn) => {
    const selected = items.filter(i => i.cb.checked).map(i => i.path);
    if (!selected.length) throw new Error('No repositories selected');
    if (btn) btn.textContent = `Importing ${selected.length} repo(s)…`;
    const current = S.settings.repos || [];
    const merged = Array.from(new Set([...current, ...selected]));
    await saveSettings({ repos: merged });
    S.settings = await call('vault', 'settings');
    // Run loadRepos and open the first repo asynchronously so the modal closes immediately
    setTimeout(async () => {
      await loadRepos();
      if (!S.repo && selected.length) {
        await openRepo(selected[0]);
      } else {
        render();
      }
      toast(`Imported ${selected.length} repository(ies) from GitHub Desktop`);
    }, 10);
    return true;
  } }], { wide: true });
}

async function cloneTo(url, depth) {
  const base = S.settings.cloneDir || (await call('sys', 'defaultCloneDir'));
  const name = url.replace(/\.git$/, '').split(/[/:]/).pop();
  const dest = base + '\\' + name;
  await G.clone(url, dest, P, depth);
  const top = await call('repo', 'add', dest);
  S.settings = await call('vault', 'settings');
  await loadRepos();
  await openRepo(top);
}

async function cloneDialog() {
  const accounts = await call('vault', 'listAccounts');
  const tabs = h('div', { class: 'tabs2' });
  const content = h('div', { style: 'display:flex;flex-direction:column;gap:8px' });
  const url = h('input', { placeholder: 'https://… or git@host:namespace/repo.git' });
  const dir = h('input', { value: S.settings.cloneDir || (await call('sys', 'defaultCloneDir')) });
  const shallow = h('input', { type: 'checkbox' });
  const depth = h('input', { type: 'number', value: 50, min: 1, style: 'width:80px' });
  const proto = h('select', {}, h('option', { value: 'ssh' }, 'SSH'), h('option', { value: 'https' }, 'HTTPS'));
  const showUrl = () => { content.innerHTML = ''; content.append(field('Repository URL', url)); };
  const showAcct = async a => {
    throbberInto(content, 'Loading repositories…', a.label || a.username || a.host, { small: true, emoji: '📦' });
    try {
      const repos = await call('api', 'repos', a.id);
      const f = h('input', { placeholder: 'Filter' });
      const l = h('div', { style: 'max-height:280px;overflow:auto;border:1px solid var(--border);border-radius:6px' });
      const draw = () => {
        l.innerHTML = '';
        const q = f.value.toLowerCase();
        const byNs = {};
        repos.filter(r => r.fullPath.toLowerCase().includes(q)).forEach(r => (byNs[r.namespace] ||= []).push(r));
        Object.keys(byNs).sort().forEach(n => {
          l.append(h('div', { class: 'mh', style: 'padding:6px 10px;color:var(--fg2);font-size:11px' }, n));
          byNs[n].forEach(r => l.append(h('div', { class: 'row', onclick: e => {
            l.querySelectorAll('.sel').forEach(x => x.classList.remove('sel')); e.currentTarget.classList.add('sel');
            url.value = proto.value === 'ssh' ? r.sshUrl : r.httpsUrl; url.dataset.ssh = r.sshUrl; url.dataset.https = r.httpsUrl;
          } }, r.private ? '🔒' : '📖', h('span', { style: 'flex:1' }, r.name), h('span', { class: 'muted' }, ago(r.updated / 1000)))));
        });
      };
      f.oninput = draw; draw();
      content.innerHTML = '';
      content.append(f, l, field('Protocol', proto));
    } catch (e) { content.innerHTML = ''; content.append(h('div', { class: 'muted' }, e.message)); }
  };
  proto.onchange = () => { if (url.dataset[proto.value]) url.value = url.dataset[proto.value]; };
  const mkTab = (label, fn) => {
    const b = h('button', { onclick: () => { tabs.querySelectorAll('button').forEach(x => x.classList.remove('active')); b.classList.add('active'); fn(); } }, label);
    tabs.append(b);
    return b;
  };
  accounts.forEach(a => mkTab(`${a.provider === 'github' ? 'GitHub' : a.provider === 'gitgud' ? 'Gitgud' : a.provider === 'gitlab' ? 'GitLab' : a.host}${a.username ? ' · ' + a.username : ''}`, () => showAcct(a)));
  mkTab('URL', showUrl).click();
  await modal('Clone a repository', [tabs, content,
    field('Clone into folder', h('div', { class: 'field-row' }, dir, h('button', { style: 'flex:none', onclick: async () => { const d = await call('sys', 'pickFolder'); if (d) dir.value = d; } }, 'Choose…'))),
    h('label', { class: 'check' }, shallow, 'Shallow clone (good for slow connections) — depth', depth)],
  [{ label: 'Cancel' }, { label: 'Clone', primary: true, action: async () => {
    if (!url.value.trim()) throw new Error('Pick a repository or enter a URL');
    if (dir.value !== S.settings.cloneDir) await saveSettings({ cloneDir: dir.value });
    task('Cloning', () => cloneTo(url.value.trim(), shallow.checked ? +depth.value : 0), { refresh: false });
  } }], { wide: true });
}

async function createDialog() {
  if (!(await ensureIdentity())) return;
  const name = h('input');
  const dir = h('input', { value: S.settings.cloneDir || (await call('sys', 'defaultCloneDir')) });
  const readme = h('input', { type: 'checkbox', checked: true });
  const ignore = h('select', {}, ['None', 'Node', 'Python', 'VisualStudio', 'Java', 'Go', 'Rust', 'Unity'].map(x => h('option', {}, x)));
  const license = h('select', {}, ['None', 'MIT', 'Unlicense'].map(x => h('option', {}, x)));
  const branch = h('input', { value: 'main' });
  const pub = h('input', { type: 'checkbox' });
  await modal('Create a new repository', [field('Name', name),
    field('Local path', h('div', { class: 'field-row' }, dir, h('button', { style: 'flex:none', onclick: async () => { const d = await call('sys', 'pickFolder'); if (d) dir.value = d; } }, 'Choose…'))),
    field('Default branch', branch), h('label', { class: 'check' }, readme, 'Initialize with a README'),
    field('Git ignore', ignore), field('License', license),
    h('label', { class: 'check' }, pub, 'Also create it on a hosting service afterwards')],
  [{ label: 'Cancel' }, { label: 'Create', primary: true, action: async () => {
    if (!name.value.trim()) throw new Error('Name required');
    const path = dir.value.replace(/[\\/]$/, '') + '\\' + name.value.trim();
    const top = await call('repo', 'create', path, { readme: readme.checked, gitignore: IGNORES[ignore.value], license: licenseText(license.value), branch: branch.value });
    S.settings = await call('vault', 'settings');
    await loadRepos(); await openRepo(top);
    if (pub.checked) setTimeout(() => publishDialog(), 50);
  } }]);
}

const IGNORES = {
  None: '', Node: 'node_modules/\ndist/\n.env\n*.log\n', Python: '__pycache__/\n*.py[cod]\n.venv/\n.env\ndist/\n*.egg-info/\n',
  VisualStudio: '.vs/\nbin/\nobj/\n*.user\n*.suo\n', Java: '*.class\ntarget/\nbuild/\n.gradle/\n.idea/\n',
  Go: 'bin/\n*.exe\n*.test\nvendor/\n', Rust: 'target/\n', Unity: '[Ll]ibrary/\n[Tt]emp/\n[Oo]bj/\n[Bb]uild/\n[Ll]ogs/\n*.csproj\n*.sln\n',
};
function licenseText(kind) {
  const who = S.settings.gitName || 'the authors';
  if (kind === 'MIT') return `MIT License\n\nCopyright (c) ${new Date().getFullYear()} ${who}\n\nPermission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\n\nThe above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\n\nTHE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n`;
  if (kind === 'Unlicense') return 'This is free and unencumbered software released into the public domain.\n\nFor more information, please refer to <https://unlicense.org>\n';
  return '';
}

/* ---------- accounts, keys, secrets ---------- */
async function accountsPage() {
  const tabs = h('div', { class: 'tabs2' });
  const body = h('div');
  const tab = (label, fn) => { const b = h('button', { onclick: () => { tabs.querySelectorAll('button').forEach(x => x.classList.remove('active')); b.classList.add('active'); body.innerHTML = ''; fn(body); } }, label); tabs.append(b); return b; };
  tab('Accounts', drawAccounts).click();
  tab('SSH keys', drawKeys);
  tab('Tokens & secrets', drawSecrets);
  modal('Accounts, keys & tokens', [tabs, body], [], { wide: true });
}

// Hosts whose SSH endpoint differs from the web host.
const SSH_HOSTS = { 'gitgud.io': 'ssh.gitgud.io' };
const info = (title, text) => modal(title, h('pre', { class: 'log' }, text), [{ label: 'OK', primary: true }]);

const PROVIDER_NAMES ={ github: 'GitHub.com', gitlab: 'GitLab.com', gitgud: 'Gitgud.io', custom: 'Other GitLab host' };

async function drawAccounts(el) {
  throbberInto(el, 'Loading accounts…', 'Checking the vault', { small: true, emoji: '🔑' });
  const accts = await call('vault', 'listAccounts');
  el.innerHTML = '';
  el.append(h('div', { class: 'toolbar-row' }, h('button', { class: 'primary', onclick: () => editAccount({}, el) }, 'Add account')),
    h('p', { class: 'muted' }, 'Tokens are encrypted with Windows DPAPI. HTTPS remotes use the token; SSH remotes use the assigned key via Windows OpenSSH.'));
  if (!accts.length) el.append(h('p', {}, 'No accounts yet.'));
  accts.forEach(a => el.append(h('div', { class: 'card' },
    h('div', { class: 'title' }, `${PROVIDER_NAMES[a.provider] || a.host}${a.label ? ' — ' + a.label : ''}`),
    h('div', { class: 'muted' }, `${a.username || '(no username)'} @ ${a.host} · token: ${a.hasToken ? 'saved' : 'none'} · SSH key: ${a.sshKey ? a.sshKey.split(/[\\/]/).pop() : 'default'}`),
    h('div', { class: 'actions' },
      h('button', { onclick: () => editAccount(a, el) }, 'Edit'),
      h('button', { onclick: async e => {
        const b = e.currentTarget; b.disabled = true; b.textContent = 'Testing…';
        try { const me = await call('api', 'me', a.id); info('Token works', `Signed in to ${a.host} as ${me.username}.`); }
        catch (err) { info('Token test failed', err.message.replace(/^.*Error: /, '')); }
        b.disabled = false; b.textContent = 'Test token';
      } }, 'Test token'),
      a.hasToken && h('button', { onclick: async () => {
        const token = await call('vault', 'revealToken', a.id);
        if (token) modal('Token — ' + (a.label || a.username || a.host), h('pre', { class: 'log mono', style: 'user-select:all' }, token),
          [{ label: 'Copy', action: () => { call('sys', 'copy', token); status('Token copied to clipboard'); } }, { label: 'Close', primary: true }]);
        else toast('No token stored for this account');
      } }, 'Reveal Token'),
      h('button', { onclick: async e => {
        const b = e.currentTarget; b.disabled = true; b.textContent = 'Testing…';
        const host = SSH_HOSTS[a.host] || a.host;
        const out = await call('ssh', 'test', host, a.sshKey || undefined);
        const ok = /welcome|successfully authenticated/i.test(out);
        info(ok ? 'SSH works' : 'SSH test failed', `git@${host}${a.sshKey ? ' using ' + a.sshKey.split(/[\\/]/).pop() : ' using default keys'}\n\n${out}`);
        b.disabled = false; b.textContent = 'Test SSH';
      } }, 'Test SSH'),
      h('button', { onclick: async () => {
        if (!a.sshKey) {
          // Show all keys that would be tried by default
          const keys = await call('ssh', 'list');
          if (!keys.length) return toast('No SSH keys found. This account uses default key discovery, but no keys exist in ~/.ssh.');
          const body = h('div', {}, h('p', { class: 'muted' }, 'This account uses default key discovery. All keys in ~/.ssh will be tried:'),
            keys.map(k => h('div', { class: 'card' },
              h('div', { class: 'title' }, k.name, h('span', { class: 'muted' }, `  ${k.type}`)),
              h('div', { class: 'mono muted', style: 'word-break:break-all;font-size:11px' }, k.publicKey))));
          modal('SSH Keys — ' + (a.label || a.username || a.host), body, [{ label: 'Close', primary: true }]);
        } else {
          const keys = await call('ssh', 'list');
          const k = keys.find(k => k.path === a.sshKey);
          if (!k) return toast(`Assigned key not found: ${a.sshKey}`);
          modal('SSH Key — ' + k.name, h('div', {},
            h('div', { class: 'muted', style: 'margin-bottom:8px' }, `Path: ${k.path}`),
            h('div', { class: 'muted', style: 'margin-bottom:8px' }, `Type: ${k.type}  ·  Comment: ${k.comment}`),
            h('div', { class: 'muted', style: 'margin-bottom:8px' }, `Fingerprint: ${k.fingerprint}`),
            h('pre', { class: 'log mono', style: 'user-select:all' }, k.publicKey)),
            [{ label: 'Copy public key', action: () => { call('sys', 'copy', k.publicKey); status('Public key copied'); } }, { label: 'Close', primary: true }]);
        }
      } }, 'Reveal SSH Key'),
      a.sshKey && h('button', { onclick: async () => {
        const k = (await call('ssh', 'list')).find(k => k.path === a.sshKey);
        try { await call('api', 'addSshKey', a.id, `KillerGit ${k.name}`, k.publicKey); info('Key uploaded', `${k.name}.pub was added to your ${a.host} account.`); }
        catch (e) { info('Upload failed', e.message.replace(/^.*Error: /, '')); }
      } }, 'Upload SSH key to account'),
      h('button', { class: 'danger', onclick: async () => { if (await confirmBox('Remove account', 'Remove this account and its saved token?', 'Remove', true)) { await call('vault', 'removeAccount', a.id); drawAccounts(el); } } }, 'Remove')))));
}

async function editAccount(a, el) {
  const keys = await call('ssh', 'list');
  const provider = h('select', {}, Object.entries(PROVIDER_NAMES).map(([k, v]) => h('option', { value: k, selected: a.provider === k }, v)));
  const host = h('input', { value: a.host || '', placeholder: 'gitlab.example.com' });
  const api = h('input', { value: a.api || '', placeholder: 'https://gitlab.example.com/api/v4' });
  const customBox = h('div', { style: 'display:flex;flex-direction:column;gap:10px' }, field('Host', host), field('API URL', api));
  const syncCustom = () => (customBox.style.display = provider.value === 'custom' ? '' : 'none');
  provider.onchange = syncCustom; syncCustom();
  const user = h('input', { value: a.username || '' });
  const label = h('input', { value: a.label || '', placeholder: 'e.g. Work' });
  const token = h('input', { type: 'password', placeholder: a.hasToken ? '(unchanged)' : 'Personal access token' });
  const key = h('select', {}, h('option', { value: '' }, 'Default (ssh-agent / ~/.ssh/config)'), keys.map(k => h('option', { value: k.path, selected: a.sshKey === k.path }, `${k.name} (${k.type})`)));
  const hint = h('p', { class: 'muted' }, 'Token scopes: GitHub → repo, read:org, admin:public_key. GitLab/Gitgud → api.');
  await modal(a.id ? 'Edit account' : 'Add account', [field('Service', provider), customBox, field('Username', user), field('Label', label), field('Personal access token', token), hint, field('SSH key for this host', key)],
    [{ label: 'Cancel' }, { label: 'Save', primary: true, action: async () => {
      const id = await call('vault', 'saveAccount', { id: a.id, provider: provider.value, host: provider.value === 'custom' ? host.value.trim() : undefined,
        api: provider.value === 'custom' ? api.value.trim() : undefined, username: user.value.trim(), label: label.value.trim(), token: token.value.trim(), sshKey: key.value });
      if (!user.value.trim() && (token.value || a.hasToken)) {
        try { const me = await call('api', 'me', id); await call('vault', 'saveAccount', { id, provider: provider.value, host: provider.value === 'custom' ? host.value.trim() : undefined, api: provider.value === 'custom' ? api.value.trim() : undefined, username: me.username, label: label.value.trim(), sshKey: key.value }); } catch {}
      }
    } }]);
  accountsPage();
}

async function drawKeys(el) {
  throbberInto(el, 'Loading SSH keys…', 'Looking in ~/.ssh', { small: true, emoji: '🗝️' });
  const keys = await call('ssh', 'list');
  el.innerHTML = '';
  el.append(h('div', { class: 'toolbar-row' },
    h('button', { class: 'primary', onclick: () => genKey(el) }, 'Generate key'),
    h('button', { onclick: () => importKey(el) }, 'Import private key'),
    h('button', { onclick: () => importLocalSshKeys(el) }, 'Import local SSH keys'),
    h('button', { onclick: async () => modal('ssh-agent', h('pre', { class: 'log' }, await call('ssh', 'agentList')), [{ label: 'OK', primary: true }]) }, 'Agent keys')),
    h('p', { class: 'muted' }, 'Keys live in %USERPROFILE%\\.ssh and are used through Windows OpenSSH. To use ssh-agent, enable the "OpenSSH Authentication Agent" service.'));
  if (!keys.length) el.append(h('p', {}, 'No keys found.'));
  keys.forEach(k => el.append(h('div', { class: 'card' },
    h('div', { class: 'title' }, k.name, h('span', { class: 'muted' }, `  ${k.type}  ${k.comment}`)),
    h('div', { class: 'mono muted', style: 'word-break:break-all' }, k.fingerprint),
    h('div', { class: 'actions' },
      h('button', { class: 'primary', onclick: () => { call('sys', 'copy', k.publicKey); status('Public key copied'); } }, 'Copy public key'),
      h('button', { onclick: async () => { const c = await prompt('Edit comment', 'Comment', k.comment); if (c != null) { try { await call('ssh', 'setComment', k.path, c); } catch (e) { toast(e.message, true); } drawKeys(el); } } }, 'Edit comment'),
      h('button', { onclick: () => changePass(k) }, 'Change passphrase'),
      h('button', { onclick: async () => { const n = await prompt('Rename key', 'New file name', k.name); if (n && n !== k.name) { try { await call('ssh', 'rename', k.path, n); } catch (e) { toast(e.message, true); } drawKeys(el); } } }, 'Rename'),
      h('button', { onclick: async () => { try { toast(await call('ssh', 'addToAgent', k.path) || 'Added to agent'); } catch (e) { toast(e.message, true); } } }, 'Add to agent'),
      h('button', { onclick: () => call('sys', 'showItem', k.path) }, 'Show file'),
      h('button', { class: 'danger', onclick: async () => { if (await confirmBox('Delete key', `Delete ${k.name} and ${k.name}.pub? This cannot be undone.`, 'Delete', true)) { await call('ssh', 'remove', k.path); drawKeys(el); } } }, 'Delete')))));
}

async function genKey(el) {
  const name = h('input', { value: 'id_ed25519_killergit' });
  const type = h('select', {}, h('option', { value: 'ed25519' }, 'Ed25519 (recommended)'), h('option', { value: 'rsa' }, 'RSA 4096'), h('option', { value: 'ecdsa' }, 'ECDSA'));
  const comment = h('input', { value: S.settings.gitEmail || '' });
  const pass = h('input', { type: 'password', placeholder: 'Optional' });
  const save = h('input', { type: 'checkbox', checked: true });
  await modal('Generate SSH key', [field('File name', name), field('Type', type), field('Comment', comment), field('Passphrase', pass), h('label', { class: 'check' }, save, 'Store passphrase in KillerGit vault')],
    [{ label: 'Cancel' }, { label: 'Generate', primary: true, action: async () => {
      const p = await call('ssh', 'generate', { name: name.value.trim(), type: type.value, comment: comment.value, passphrase: pass.value });
      if (pass.value && save.checked) await call('vault', 'saveSecret', { name: `SSH passphrase: ${name.value}`, kind: 'ssh-passphrase', value: pass.value });
      toast(`Key created at ${p}`);
    } }]);
  drawKeys(el);
}

async function importKey(el) {
  const name = h('input', { placeholder: 'id_ed25519_work' });
  const key = h('textarea', { rows: 8, class: 'mono', placeholder: '-----BEGIN OPENSSH PRIVATE KEY-----' });
  const file = h('button', { onclick: async () => { const f = await call('sys', 'pickFile', 'Private key file'); if (f) { key.value = await call('sys', 'readFileText', f); if (!name.value) name.value = f.split(/[\\/]/).pop(); } } }, 'Load from file…');
  await modal('Import private key', [field('Save as', name), field('Private key', key), file],
    [{ label: 'Cancel' }, { label: 'Import', primary: true, action: () => call('ssh', 'import', name.value.trim(), key.value) }]);
  drawKeys(el);
}

async function changePass(k) {
  const o = h('input', { type: 'password' }), n = h('input', { type: 'password' });
  await modal(`Change passphrase — ${k.name}`, [field('Current passphrase (blank if none)', o), field('New passphrase (blank to remove)', n)],
    [{ label: 'Cancel' }, { label: 'Change', primary: true, action: async () => { await call('ssh', 'changePassphrase', k.path, o.value, n.value); toast('Passphrase changed'); } }]);
}

async function importLocalSshKeys(el, customFolder) {
  let folder = customFolder;
  let localKeys = await call('sys', 'localSshKeys', folder);
  if (!localKeys.length && !folder) {
    const pick = await confirmBox('No keys in ~/.ssh', 'No SSH keys were found in %USERPROFILE%\\.ssh.\n\nWould you like to choose another folder to scan for SSH keys?', 'Choose folder…');
    if (pick) {
      folder = await call('sys', 'pickFolder', 'Select folder containing SSH keys');
      if (folder) localKeys = await call('sys', 'localSshKeys', folder);
    }
  }
  if (!localKeys.length) return toast(folder ? `No SSH keys found in ${folder}.` : 'No SSH keys found in ~/.ssh.');
  const existing = await call('ssh', 'list');
  const existingNames = new Set(existing.map(k => k.name));
  const newKeys = folder ? localKeys : localKeys.filter(k => !existingNames.has(k.name));
  if (!newKeys.length) return toast('All SSH keys in that folder are already known to KillerGit.');
  const items = newKeys.map(k => {
    const cb = h('input', { type: 'checkbox', checked: true });
    return { key: k, cb, el: h('div', { class: 'row', onclick: () => (cb.checked = !cb.checked) },
      cb, h('span', { style: 'flex:1' }, k.name), h('span', { class: 'muted' }, k.type),
      h('span', { class: 'muted' }, k.hasPrivate ? '(has private key)' : '(public only)')) };
  });
  const list = h('div', { style: 'max-height:320px;overflow:auto;border:1px solid var(--border);border-radius:6px' }, items.map(i => i.el));
  const accts = await call('vault', 'listAccounts');
  const assignTo = h('select', {}, h('option', { value: '' }, '— None —'), accts.map(a => h('option', { value: a.id }, `${a.label || a.username || a.provider} — ${a.host}`)));
  await modal('Import local SSH keys', [
    h('p', { class: 'muted' }, `Found ${localKeys.length} key(s) in ${folder || '~/.ssh'}, ${newKeys.length} new to KillerGit.`),
    list,
    accts.length ? field('Assign selected keys to account (optional)', assignTo) : null,
  ], [{ label: 'Cancel' }, { label: 'Import', primary: true, action: async () => {
    const selected = items.filter(i => i.cb.checked);
    if (!selected.length) throw new Error('No keys selected');
    let imported = 0;
    for (const item of selected) {
      let keyPath = item.key.path;
      if (folder && item.key.hasPrivate) {
        const privContent = await call('sys', 'readFileText', item.key.path);
        await call('ssh', 'import', item.key.name, privContent);
        keyPath = item.key.path;
      } else if (!item.key.pubPath && item.key.hasPrivate) {
        const privContent = await call('sys', 'readFileText', item.key.path);
        await call('ssh', 'import', item.key.name, privContent);
      }
      if (assignTo.value && item.key.hasPrivate) {
        const acct = accts.find(a => a.id === assignTo.value);
        if (acct) await call('vault', 'saveAccount', { id: acct.id, provider: acct.provider, sshKey: keyPath });
      }
      imported++;
    }
    toast(`Imported / recognized ${imported} SSH key(s)`);
    drawKeys(el);
    return true;
  } }]);
}

async function drawSecrets(el) {
  throbberInto(el, 'Unlocking secrets…', 'Decrypting the vault', { small: true, emoji: '🤫' });
  const list = await call('vault', 'listSecrets');
  el.innerHTML = '';
  el.append(h('div', { class: 'toolbar-row' }, h('button', { class: 'primary', onclick: () => editSecret({}, el) }, 'Add secret')),
    h('p', { class: 'muted' }, 'Store extra personal access tokens, deploy keys, passphrases or notes. Encrypted at rest.'));
  if (!list.length) el.append(h('p', {}, 'Nothing stored.'));
  list.forEach(s => el.append(h('div', { class: 'card' }, h('div', { class: 'title' }, s.name), h('div', { class: 'muted' }, s.kind),
    h('div', { class: 'actions' },
      h('button', { class: 'primary', onclick: async () => { call('sys', 'copy', await call('vault', 'revealSecret', s.id)); status('Copied to clipboard'); } }, 'Copy'),
      h('button', { onclick: async () => modal(s.name, h('pre', { class: 'log mono' }, await call('vault', 'revealSecret', s.id)), [{ label: 'Close', primary: true }]) }, 'Reveal'),
      h('button', { onclick: () => editSecret(s, el) }, 'Edit'),
      h('button', { class: 'danger', onclick: async () => { if (await confirmBox('Delete secret', `Delete ${s.name}?`, 'Delete', true)) { await call('vault', 'removeSecret', s.id); drawSecrets(el); } } }, 'Delete')))));
}

async function editSecret(s, el) {
  const name = h('input', { value: s.name || '' });
  const kind = h('select', {}, ['token', 'ssh-passphrase', 'password', 'note'].map(k => h('option', { selected: s.kind === k }, k)));
  const value = h('textarea', { rows: 4, class: 'mono', placeholder: s.id ? '(unchanged)' : '' });
  await modal(s.id ? 'Edit secret' : 'Add secret', [field('Name', name), field('Kind', kind), field('Value', value)],
    [{ label: 'Cancel' }, { label: 'Save', primary: true, action: () => call('vault', 'saveSecret', { id: s.id, name: name.value, kind: kind.value, value: value.value }) }]);
  accountsPage();
}

/* ---------- settings ---------- */
async function settingsPage() {
  const s = S.settings;
  const theme = h('select', {}, ['system', 'light', 'dark'].map(t => h('option', { value: t, selected: (s.theme || 'system') === t }, t[0].toUpperCase() + t.slice(1))));
  theme.onchange = () => saveSettings({ theme: theme.value });
  const sw = h('div', { class: 'swatches' });
  const custom = h('input', { type: 'color', value: s.accent || ACCENTS[0] });
  const drawSw = () => {
    sw.innerHTML = '';
    ACCENTS.forEach(c => sw.append(h('div', { class: 'swatch' + ((S.settings.accent || ACCENTS[0]) === c ? ' sel' : ''), style: `background:${c}`, onclick: async () => { await saveSettings({ accent: c }); custom.value = c; drawSw(); } })));
    sw.append(custom);
  };
  custom.oninput = () => { document.documentElement.style.setProperty('--accent', custom.value); };
  custom.onchange = async () => { await saveSettings({ accent: custom.value }); drawSw(); };
  drawSw();
  const name = h('input', { value: await G.config(null, 'user.name') });
  const email = h('input', { value: await G.config(null, 'user.email') });
  const editor = h('input', { value: s.editor || 'code', placeholder: 'code' });
  const cloneDir = h('input', { value: s.cloneDir || (await call('sys', 'defaultCloneDir')) });
  const rebase = h('input', { type: 'checkbox', checked: !!s.pullRebase });
  const chunked = h('input', { type: 'checkbox', checked: !!s.chunked });
  const chunkN = h('input', { type: 'number', min: 1, value: chunkSize() });
  const useWinSsh = h('input', { type: 'checkbox', checked: s.globalSsh !== false });
  const sshOk = await call('sys', 'sshAvailable');
  await modal('Settings', [
    h('h3', { style: 'margin:0' }, 'Appearance'), field('Theme', theme), field('Accent color', sw),
    h('h3', { style: 'margin:8px 0 0' }, 'Git'), field('Name', name), field('Email', email),
    h('label', { class: 'check' }, rebase, 'Pull with rebase by default'),
    h('label', { class: 'check' }, useWinSsh, `Set Windows OpenSSH as git's global core.sshCommand ${sshOk ? '' : '(not installed!)'}`),
    h('h3', { style: 'margin:8px 0 0' }, 'Slow / outdated connections'),
    h('label', { class: 'check' }, chunked, 'Pull and push in chunks, with automatic retries'),
    field('Commits per chunk', chunkN),
    h('h3', { style: 'margin:8px 0 0' }, 'Integrations'), field('Editor command', editor), field('Default clone folder', cloneDir),
  ], [{ label: 'Cancel' }, { label: 'Save', primary: true, action: async () => {
    if (name.value) await G.setConfig(null, 'user.name', name.value);
    if (email.value) await G.setConfig(null, 'user.email', email.value);
    if (useWinSsh.checked && sshOk) await G.setConfig(null, 'core.sshCommand', 'C:/Windows/System32/OpenSSH/ssh.exe');
    await saveSettings({ editor: editor.value, cloneDir: cloneDir.value, pullRebase: rebase.checked, chunked: chunked.checked,
      chunkSize: +chunkN.value || 50, gitName: name.value, gitEmail: email.value, globalSsh: useWinSsh.checked });
    renderToolbar();
  } }]);
}

/* ---------- boot ---------- */
async function boot() {
  S.settings = await call('vault', 'settings');
  applyTheme();
  const side = $('#sidebar');
  if (+S.settings.sidebarWidth) side.style.width = Math.max(180, Math.min(560, +S.settings.sidebarWidth)) + 'px';
  installResizer($('#sidebar-resizer'), side, {
    min: 180,
    max: () => Math.min(560, innerWidth * 0.45),
    reset: '270px',
    onEnd: w => saveSettings({ sidebarWidth: w }),
  });
  $('#tb-repo').onclick = () => $('#repo-filter').focus();
  $('#tb-branch').onclick = e => branchMenu(e.currentTarget);
  $('#tb-sync').onclick = syncPrimary;
  $('#tb-sync').oncontextmenu = e => { e.preventDefault(); syncMenu(e.currentTarget); };
  $('#tb-sync-sub').title = 'Right-click for more';
  const needRemote = fn => () => { const r = currentRemote(); if (!S.repo || !r) return toast('This repository has no remotes. Add one under Remotes.'); fn(r); };
  $('#tb-pull').onclick = needRemote(r => doPull(r));
  $('#tb-push').onclick = needRemote(r => doPush(r));
  $('#tb-more').onclick = e => syncMenu(e.currentTarget);
  $('#tb-remote').onclick = e => remoteMenu(e.currentTarget);
  $('#btn-settings').onclick = settingsPage;
  $('#btn-accounts').onclick = accountsPage;
  $('#btn-add').onclick = e => addMenu(e.currentTarget);
  $('#repo-filter').oninput = renderTree;
  $('#repo-sort').onchange = e => saveSettings({ repoSort: e.target.value }).then(renderTree);
  $('#status-log').onclick = showLog;
  document.querySelectorAll('#tabs button').forEach(b => (b.onclick = () => setTab(b.dataset.tab)));
  window.addEventListener('focus', () => { if (S.repo && !busy && S.tab === 'changes') refreshRepo(); });
  document.addEventListener('keydown', e => {
    if (!e.ctrlKey) return;
    const k = e.key.toLowerCase();
    if (k === '1') setTab('changes');
    else if (k === '2') setTab('history');
    else if (k === 'p' && e.shiftKey) syncPrimary();
    else if (k === 'b') branchMenu($('#tb-branch'));
    else if (k === 'o') addLocal();
    else if (k === 'n') createDialog();
    else if (k === 'r' && e.shiftKey) refreshRepo();
    else return;
    e.preventDefault();
  });
  await loadRepos();
  if (S.settings.lastRepo && (S.settings.repos || []).includes(S.settings.lastRepo)) openRepo(S.settings.lastRepo);
  else render();
}

boot();
