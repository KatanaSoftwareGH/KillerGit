// Thin wrapper around the git CLI. Every call is a child process; nothing is cached here.
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const vault = require('./vault');
const ssh = require('./ssh');

const SEP = '\x1f';
const REC = '\x1e';

// Builds per-call auth: HTTPS gets an Authorization header from the vault,
// SSH gets Windows' native OpenSSH plus the key assigned to that host.
function authFor(url) {
  const args = [];
  const env = {};
  if (!url) return { args, env };
  const host = hostOf(url);
  const acct = vault.accountForHost(host);
  if (/^https?:/i.test(url) && acct && acct.token) {
    const user = acct.kind === 'github' ? 'x-access-token' : 'oauth2';
    const b64 = Buffer.from(`${acct.username || user}:${acct.token}`).toString('base64');
    args.push('-c', `http.extraHeader=Authorization: Basic ${b64}`);
  }
  // git runs in a hidden console, so ssh must never prompt (unknown host key, passphrase):
  // a prompt nobody can see looks like an endless push. Fail fast and surface the error instead.
  let sshCmd = `"${ssh.SSH_EXE.replace(/\\/g, '/')}" -o BatchMode=yes -o StrictHostKeyChecking=accept-new` +
    ' -o ConnectTimeout=20 -o ServerAliveInterval=15 -o ServerAliveCountMax=4';
  if (acct && acct.sshKey) sshCmd += ` -i "${acct.sshKey.replace(/\\/g, '/')}" -o IdentitiesOnly=yes`;
  env.GIT_SSH_COMMAND = sshCmd;
  return { args, env };
}

function hostOf(url) {
  let m = url.match(/^[a-z+]+:\/\/(?:[^@/]+@)?([^/:]+)/i);
  if (m) return m[1].toLowerCase();
  m = url.match(/^(?:[^@]+@)?([^:]+):/);
  return m ? m[1].toLowerCase() : '';
}

function run(cwd, args, opts = {}) {
  return new Promise((resolve, reject) => {
    const { args: authArgs, env } = authFor(opts.url);
    const p = spawn('git', [...authArgs, ...args], {
      cwd,
      env: { ...process.env, ...env, GIT_TERMINAL_PROMPT: '0', LC_ALL: 'C' },
      windowsHide: true,
    });
    let out = '', err = '';
    p.stdout.on('data', d => { out += d; });
    p.stderr.on('data', d => { err += d; if (opts.onProgress) opts.onProgress(String(d)); });
    if (opts.input) { p.stdin.write(opts.input); p.stdin.end(); }
    p.on('error', reject);
    p.on('close', code => {
      if (code === 0 || opts.allowFail) resolve(out);
      else reject(new Error(err.trim() || `git ${args[0]} exited ${code}`));
    });
  });
}

async function remoteUrl(cwd, remote) {
  return (await run(cwd, ['remote', 'get-url', remote], { allowFail: true })).trim();
}

async function status(cwd) {
  const out = await run(cwd, ['status', '--porcelain=v1', '-z', '-b', '-uall']);
  const parts = out.split('\0');
  const files = [];
  let branch = '', upstream = '', ahead = 0, behind = 0;
  for (let i = 0; i < parts.length; i++) {
    const e = parts[i];
    if (!e) continue;
    if (e.startsWith('## ')) {
      const m = e.slice(3).match(/^(.+?)(?:\.\.\.(\S+))?(?: \[(.*)\])?$/);
      branch = m[1].replace('No commits yet on ', '');
      upstream = m[2] || '';
      const a = (m[3] || '').match(/ahead (\d+)/), b = (m[3] || '').match(/behind (\d+)/);
      ahead = a ? +a[1] : 0; behind = b ? +b[1] : 0;
      continue;
    }
    const x = e[0], y = e[1];
    let file = e.slice(3), orig;
    if (x === 'R' || x === 'C') orig = parts[++i];
    files.push({ path: file, orig, index: x, work: y, staged: x !== ' ' && x !== '?', unstaged: y !== ' ' });
  }
  return { branch, upstream, ahead, behind, files };
}

async function diff(cwd, file, staged, untracked) {
  if (untracked) return run(cwd, ['diff', '--no-index', '--', '/dev/null', file], { allowFail: true });
  return run(cwd, ['diff', ...(staged ? ['--cached'] : []), '--', file]);
}

async function log(cwd, { skip = 0, limit = 400, all = true, branch } = {}) {
  const fmt = ['%H', '%P', '%an', '%ae', '%at', '%D', '%s'].join(SEP) + REC;
  const out = await run(cwd, ['log', ...(all ? ['--all'] : [branch || 'HEAD']), '--topo-order',
    `--skip=${skip}`, `-n${limit}`, `--format=${fmt}`], { allowFail: true });
  return out.split(REC).map(s => s.replace(/^\n/, '')).filter(Boolean).map(r => {
    const [hash, parents, author, email, time, refs, subject] = r.split(SEP);
    return { hash, parents: parents ? parents.split(' ') : [], author, email, time: +time,
      refs: refs ? refs.split(', ') : [], subject };
  });
}

async function show(cwd, hash) {
  const meta = await run(cwd, ['show', '-s', `--format=%H${SEP}%an <%ae>${SEP}%at${SEP}%B`, hash]);
  const [h, author, time, body] = meta.split(SEP);
  const files = (await run(cwd, ['show', '--format=', '--name-status', '-M', hash])).trim()
    .split('\n').filter(Boolean).map(l => { const p = l.split('\t'); return { status: p[0][0], path: p[p.length - 1] }; });
  return { hash: h, author, time: +time, body, files };
}

async function showFile(cwd, hash, file) {
  return run(cwd, ['show', '--format=', '-M', hash, '--', file]);
}

async function branches(cwd) {
  const fmt = ['%(refname)', '%(refname:short)', '%(upstream:short)', '%(committerdate:unix)', '%(HEAD)', '%(upstream:track)'].join(SEP);
  const out = await run(cwd, ['for-each-ref', `--format=${fmt}`, 'refs/heads', 'refs/remotes', 'refs/tags']);
  return out.trim().split('\n').filter(Boolean).map(l => {
    const [ref, name, upstream, date, head, track] = l.split(SEP);
    const type = ref.startsWith('refs/heads') ? 'local' : ref.startsWith('refs/tags') ? 'tag' : 'remote';
    return { ref, name, upstream, date: +date, current: head === '*', track, type };
  }).filter(b => !b.name.endsWith('/HEAD'));
}

async function remotes(cwd) {
  const out = await run(cwd, ['remote', '-v'], { allowFail: true });
  const map = {};
  out.trim().split('\n').filter(Boolean).forEach(l => {
    const [name, url, kind] = l.split(/\s+/);
    map[name] = map[name] || { name, fetch: '', push: '' };
    map[name][kind === '(push)' ? 'push' : 'fetch'] = url;
  });
  return Object.values(map);
}

async function withRemote(cwd, remote, fn) {
  return fn(await remoteUrl(cwd, remote));
}

const ops = {
  status, diff, log, show, showFile, branches, remotes,
  isRepo: async cwd => fs.existsSync(cwd) && (await run(cwd, ['rev-parse', '--is-inside-work-tree'], { allowFail: true })).trim() === 'true',
  topLevel: async cwd => (await run(cwd, ['rev-parse', '--show-toplevel'])).trim().replace(/\//g, path.sep),
  lastActivity: async cwd => +(await run(cwd, ['log', '-1', '--all', '--format=%ct'], { allowFail: true })).trim() || 0,
  init: (cwd, branch = 'main') => run(cwd, ['init', '-b', branch]),
  clone: (url, dest, onProgress, depth) =>
    run(path.dirname(dest), ['clone', '--progress', ...(depth ? [`--depth=${depth}`] : []), url, dest], { url, onProgress }),
  stage: (cwd, files) => run(cwd, ['add', '--', ...files]),
  stageAll: cwd => run(cwd, ['add', '-A']),
  unstage: (cwd, files) => run(cwd, ['reset', '-q', 'HEAD', '--', ...files], { allowFail: true }),
  applyPatch: (cwd, patch, { cached = true, reverse = false } = {}) =>
    run(cwd, ['apply', ...(cached ? ['--cached'] : []), ...(reverse ? ['-R'] : []), '--unidiff-zero', '-'], { input: patch }),
  discard: async (cwd, files, untracked) => {
    if (untracked.length) await run(cwd, ['clean', '-f', '--', ...untracked]);
    if (files.length) await run(cwd, ['checkout', 'HEAD', '--', ...files]);
  },
  commit: (cwd, message, { amend, signoff } = {}) => message
    ? run(cwd, ['commit', '-F', '-', ...(amend ? ['--amend'] : []), ...(signoff ? ['-s'] : [])], { input: message })
    : run(cwd, ['commit', '--amend', '--no-edit']),
  // Splits the staged set into several commits of `size` files each.
  commitChunked: async (cwd, message, size, onProgress) => {
    const st = await status(cwd);
    const staged = st.files.filter(f => f.staged).map(f => f.path);
    if (!staged.length) throw new Error('Nothing staged');
    await run(cwd, ['reset', '-q'], { allowFail: true });
    const total = Math.ceil(staged.length / size);
    for (let i = 0; i < total; i++) {
      const chunk = staged.slice(i * size, (i + 1) * size);
      await run(cwd, ['add', '-A', '--', ...chunk]);
      await run(cwd, ['commit', '-F', '-'], { input: total > 1 ? `${message}\n\n[part ${i + 1}/${total}]` : message });
      onProgress && onProgress(`Committed part ${i + 1}/${total}\n`);
    }
  },
  undoCommit: cwd => run(cwd, ['reset', '--soft', 'HEAD~1']),
  checkout: (cwd, name) => run(cwd, ['checkout', name]),
  checkoutRemote: (cwd, remoteRef, local) => run(cwd, ['checkout', '-b', local, '--track', remoteRef]),
  createBranch: (cwd, name, from) => run(cwd, ['checkout', '-b', name, ...(from ? [from] : [])]),
  renameBranch: (cwd, oldN, newN) => run(cwd, ['branch', '-m', oldN, newN]),
  deleteBranch: (cwd, name, force) => run(cwd, ['branch', force ? '-D' : '-d', name]),
  deleteRemoteBranch: (cwd, remote, name) => withRemote(cwd, remote, url => run(cwd, ['push', remote, '--delete', name], { url })),
  merge: (cwd, name, squash) => run(cwd, ['merge', ...(squash ? ['--squash'] : ['--no-edit']), name]),
  rebase: (cwd, onto) => run(cwd, ['rebase', onto]),
  abort: async cwd => {
    const g = (await run(cwd, ['rev-parse', '--git-dir'])).trim();
    const dir = path.resolve(cwd, g);
    if (fs.existsSync(path.join(dir, 'rebase-merge')) || fs.existsSync(path.join(dir, 'rebase-apply'))) return run(cwd, ['rebase', '--abort']);
    if (fs.existsSync(path.join(dir, 'CHERRY_PICK_HEAD'))) return run(cwd, ['cherry-pick', '--abort']);
    if (fs.existsSync(path.join(dir, 'REVERT_HEAD'))) return run(cwd, ['revert', '--abort']);
    return run(cwd, ['merge', '--abort']);
  },
  conflictState: async cwd => {
    const dir = path.resolve(cwd, (await run(cwd, ['rev-parse', '--git-dir'])).trim());
    if (fs.existsSync(path.join(dir, 'MERGE_HEAD'))) return 'merge';
    if (fs.existsSync(path.join(dir, 'rebase-merge')) || fs.existsSync(path.join(dir, 'rebase-apply'))) return 'rebase';
    if (fs.existsSync(path.join(dir, 'CHERRY_PICK_HEAD'))) return 'cherry-pick';
    return null;
  },
  continueOp: async (cwd, kind) => run(cwd, [kind === 'rebase' ? 'rebase' : kind, '--continue'], { allowFail: false }),
  cherryPick: (cwd, hash) => run(cwd, ['cherry-pick', hash]),
  revert: (cwd, hash) => run(cwd, ['revert', '--no-edit', hash]),
  resetTo: (cwd, hash, mode) => run(cwd, ['reset', `--${mode}`, hash]),
  tag: (cwd, name, hash, message) => run(cwd, ['tag', ...(message ? ['-a', name, '-m', message] : [name]), hash]),
  deleteTag: (cwd, name) => run(cwd, ['tag', '-d', name]),
  stash: (cwd, message) => run(cwd, ['stash', 'push', '-u', ...(message ? ['-m', message] : [])]),
  stashList: async cwd => (await run(cwd, ['stash', 'list', `--format=%gd${SEP}%s${SEP}%ct`])).trim().split('\n').filter(Boolean)
    .map(l => { const [ref, msg, t] = l.split(SEP); return { ref, msg, time: +t }; }),
  stashPop: (cwd, ref) => run(cwd, ['stash', 'pop', ref]),
  stashDrop: (cwd, ref) => run(cwd, ['stash', 'drop', ref]),
  addRemote: (cwd, name, url) => run(cwd, ['remote', 'add', name, url]),
  removeRemote: (cwd, name) => run(cwd, ['remote', 'remove', name]),
  setRemoteUrl: (cwd, name, url) => run(cwd, ['remote', 'set-url', name, url]),
  fetch: (cwd, remote, onProgress) => withRemote(cwd, remote, url => run(cwd, ['fetch', '--progress', '--prune', remote], { url, onProgress })),
  fetchAll: async (cwd, onProgress) => { for (const r of await remotes(cwd)) await ops.fetch(cwd, r.name, onProgress); },
  pull: (cwd, remote, branch, rebase, onProgress) =>
    withRemote(cwd, remote, url => run(cwd, ['pull', '--progress', rebase ? '--rebase' : '--no-rebase', remote, branch], { url, onProgress })),
  push: (cwd, remote, branch, { force, setUpstream, tags } = {}, onProgress) =>
    withRemote(cwd, remote, url => run(cwd, ['push', '--progress', ...(setUpstream ? ['-u'] : []),
      ...(force ? ['--force-with-lease'] : []), ...(tags ? ['--follow-tags'] : []), remote, branch], { url, onProgress })),

  // Pushes unpushed commits a few at a time so a dropped connection loses at most one batch.
  pushChunked: async (cwd, remote, branch, size, onProgress) => {
    const url = await remoteUrl(cwd, remote);
    const remoteRef = `${remote}/${branch}`;
    const hasRemote = (await run(cwd, ['rev-parse', '--verify', '-q', remoteRef], { allowFail: true })).trim();
    const range = hasRemote ? `${remoteRef}..HEAD` : 'HEAD';
    const list = (await run(cwd, ['rev-list', '--reverse', '--first-parent', range])).trim().split('\n').filter(Boolean);
    if (!list.length) return onProgress && onProgress('Nothing to push\n');
    const steps = [];
    for (let i = size - 1; i < list.length; i += size) steps.push(list[i]);
    if (steps[steps.length - 1] !== list[list.length - 1]) steps.push(list[list.length - 1]);
    for (let i = 0; i < steps.length; i++) {
      await retry(() => run(cwd, ['push', '--progress', remote, `${steps[i]}:refs/heads/${branch}`], { url, onProgress }), onProgress);
      onProgress && onProgress(`Pushed chunk ${i + 1}/${steps.length}\n`);
    }
    await run(cwd, ['branch', `--set-upstream-to=${remoteRef}`], { allowFail: true });
  },

  // Fetches history in depth increments (shallow repos) or with retries (full repos), then merges.
  pullChunked: async (cwd, remote, branch, size, onProgress) => {
    const url = await remoteUrl(cwd, remote);
    const shallow = (await run(cwd, ['rev-parse', '--is-shallow-repository'])).trim() === 'true';
    const cfg = ['-c', 'http.lowSpeedLimit=1000', '-c', 'http.lowSpeedTime=60', '-c', 'http.postBuffer=524288000'];
    if (shallow) {
      for (let i = 1; ; i++) {
        const before = (await run(cwd, ['rev-list', '--count', '--all'])).trim();
        await retry(() => run(cwd, [...cfg, 'fetch', '--progress', `--deepen=${size}`, remote, branch], { url, onProgress }), onProgress);
        const after = (await run(cwd, ['rev-list', '--count', '--all'])).trim();
        onProgress && onProgress(`Deepened round ${i}: ${after} commits\n`);
        if (before === after) break;
      }
    } else {
      // Walk the remote branch backwards in steps so each fetch is a bounded pack.
      await retry(() => run(cwd, [...cfg, 'fetch', '--progress', `--depth=${size}`, remote, branch], { url, onProgress }), onProgress);
      await retry(() => run(cwd, [...cfg, 'fetch', '--progress', remote, branch], { url, onProgress }), onProgress);
    }
    return run(cwd, ['merge', '--no-edit', `${remote}/${branch}`]);
  },
  unshallow: (cwd, remote, onProgress) => withRemote(cwd, remote, url => run(cwd, ['fetch', '--progress', '--unshallow', remote], { url, onProgress })),
  config: async (cwd, key) => (await run(cwd || process.cwd(), ['config', ...(cwd ? [] : ['--global']), '--get', key], { allowFail: true })).trim(),
  setConfig: (cwd, key, val) => run(cwd || process.cwd(), ['config', ...(cwd ? [] : ['--global']), key, val]),
  hostOf,
};

async function retry(fn, onProgress, tries = 5) {
  for (let i = 1; ; i++) {
    try { return await fn(); } catch (e) {
      if (i >= tries) throw e;
      onProgress && onProgress(`Attempt ${i} failed (${e.message.split('\n')[0]}), retrying…\n`);
      await new Promise(r => setTimeout(r, 1500 * i));
    }
  }
}

module.exports = ops;
