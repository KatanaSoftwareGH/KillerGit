const { app, BrowserWindow, ipcMain, dialog, shell, clipboard, nativeTheme } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');
const git = require('./git');
const ssh = require('./ssh');
const vault = require('./vault');
const providers = require('./providers');

app.setAppUserModelId('io.killergit.app');

let win;

function createWindow() {
  const s = vault.settings();
  win = new BrowserWindow({
    width: s.width || 1280, height: s.height || 800, minWidth: 900, minHeight: 560,
    icon: path.join(__dirname, '..', 'renderer', 'icon.png'),
    backgroundColor: nativeTheme.shouldUseDarkColors ? '#16181d' : '#ffffff',
    autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, spellcheck: true },
  });
  win.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));
  win.on('close', () => { const [width, height] = win.getSize(); vault.saveSettings({ width, height }); });
  win.webContents.setWindowOpenHandler(({ url }) => { shell.openExternal(url); return { action: 'deny' }; });
  win.webContents.on('before-input-event', (event, input) => {
    if (input.key === 'F12' || (input.control && input.shift && input.key.toLowerCase() === 'i')) {
      win.webContents.toggleDevTools();
      event.preventDefault();
    }
  });
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());

const progress = text => win && win.webContents.send('progress', text);

// Renderer passes '__progress__' wherever a git op takes an onProgress callback.
const withProgress = args => args.map(a => (a === '__progress__' ? progress : a));

const repoOps = {
  async summary(dir) {
    const [ok, remotes, lastActivity] = await Promise.all([
      git.isRepo(dir).catch(() => false),
      git.remotes(dir).catch(() => []),
      git.lastActivity(dir).catch(() => 0),
    ]);
    if (!ok) return { path: dir, missing: true };
    const primary = remotes.find(r => r.name === 'origin') || remotes[0];
    const parsed = primary ? providers.parseRemote(primary.fetch) : null;
    return {
      path: dir, name: path.basename(dir), remotes,
      host: parsed ? parsed.host : null, namespace: parsed ? parsed.namespace : null,
      lastActivity,
    };
  },
  // Cheap enough to run for the whole sidebar in the background. Untracked
  // directories are reported as one entry instead of expanding every file.
  async changed(dir) {
    try {
      const st = await git.status(dir, { untracked: 'normal' });
      return { changed: st.files.length, ahead: st.ahead, behind: st.behind };
    } catch {
      return { changed: 0, ahead: 0, behind: 0 };
    }
  },
  async add(dir) {
    const top = await git.topLevel(dir);
    const s = vault.settings();
    const repos = Array.from(new Set([...(s.repos || []), top]));
    vault.saveSettings({ repos });
    return top;
  },
  remove(dir) { vault.saveSettings({ repos: (vault.settings().repos || []).filter(r => r !== dir) }); },
  async create(dir, { readme, gitignore, license, branch }) {
    fs.mkdirSync(dir, { recursive: true });
    await git.init(dir, branch || 'main');
    if (readme) fs.writeFileSync(path.join(dir, 'README.md'), `# ${path.basename(dir)}\n`);
    if (gitignore) fs.writeFileSync(path.join(dir, '.gitignore'), gitignore);
    if (license) fs.writeFileSync(path.join(dir, 'LICENSE'), license);
    if (readme || gitignore || license) { await git.stageAll(dir); await git.commit(dir, 'Initial commit'); }
    return repoOps.add(dir);
  },
  readFile: (dir, f) => fs.readFileSync(path.join(dir, f), 'utf8'),
  appendIgnore: (dir, pattern) => fs.appendFileSync(path.join(dir, '.gitignore'), `\n${pattern}`),
};

function readVarint(buf, pos) {
  let res = 0, shift = 0;
  while (pos < buf.length) {
    const b = buf[pos++];
    res |= (b & 0x7f) << shift;
    if (!(b & 0x80)) break;
    shift += 7;
  }
  return [res, pos];
}

function decompressSnappy(buf) {
  try {
    let pos = 0;
    let len = 0, shift = 0;
    while (pos < buf.length) {
      const b = buf[pos++];
      len |= (b & 0x7f) << shift;
      if (!(b & 0x80)) break;
      shift += 7;
    }
    if (len <= 0 || len > 10 * 1024 * 1024) return null;
    const out = Buffer.alloc(len);
    let outPos = 0;

    while (pos < buf.length && outPos < len) {
      const tag = buf[pos++];
      const type = tag & 0x03;
      if (type === 0) {
        let litLen = (tag >> 2) + 1;
        if (litLen > 60) {
          const extraBytes = litLen - 60;
          litLen = 0;
          for (let i = 0; i < extraBytes; i++) litLen |= buf[pos++] << (i * 8);
          litLen += 1;
        }
        if (pos + litLen > buf.length || outPos + litLen > len) break;
        buf.copy(out, outPos, pos, pos + litLen);
        pos += litLen;
        outPos += litLen;
      } else if (type === 1) {
        const copyLen = ((tag >> 2) & 0x07) + 4;
        const offset = ((tag >> 5) << 8) | buf[pos++];
        if (offset === 0 || offset > outPos) break;
        for (let i = 0; i < copyLen && outPos < len; i++) {
          out[outPos] = out[outPos - offset];
          outPos++;
        }
      } else if (type === 2) {
        const copyLen = (tag >> 2) + 1;
        const offset = buf[pos] | (buf[pos + 1] << 8);
        pos += 2;
        if (offset === 0 || offset > outPos) break;
        for (let i = 0; i < copyLen && outPos < len; i++) {
          out[outPos] = out[outPos - offset];
          outPos++;
        }
      } else if (type === 3) {
        const copyLen = (tag >> 2) + 1;
        const offset = buf[pos] | (buf[pos + 1] << 8) | (buf[pos + 2] << 16) | (buf[pos + 3] << 24);
        pos += 4;
        if (offset === 0 || offset > outPos) break;
        for (let i = 0; i < copyLen && outPos < len; i++) {
          out[outPos] = out[outPos - offset];
          outPos++;
        }
      }
    }
    return out.subarray(0, outPos);
  } catch {
    return null;
  }
}

function parseLevelDbBlocks(buf) {
  if (buf.length < 48) return null;
  try {
    const magic = buf.readBigUInt64LE(buf.length - 8);
    if (magic !== 0xdb4775248b80fb57n) return null;
    const [, p1] = readVarint(buf, buf.length - 48);
    const [, p2] = readVarint(buf, p1);
    const [indexOffset, p3] = readVarint(buf, p2);
    const [indexSize] = readVarint(buf, p3);

    const indexCompressed = buf.subarray(indexOffset, indexOffset + indexSize);
    const indexType = buf[indexOffset + indexSize];
    const indexData = (indexType === 1 ? decompressSnappy(indexCompressed) : indexCompressed) || indexCompressed;

    const blocks = [];
    let ipos = 0;
    while (ipos < indexData.length - 4) {
      const [, i1] = readVarint(indexData, ipos);
      const [nonShared, i2] = readVarint(indexData, i1);
      const [valLen, i3] = readVarint(indexData, i2);
      if (valLen <= 0 || i3 + nonShared + valLen > indexData.length) break;
      ipos = i3 + nonShared;
      const [blockOffset, v1] = readVarint(indexData, ipos);
      const [blockSize] = readVarint(indexData, v1);
      ipos += valLen;
      blocks.push({ offset: blockOffset, size: blockSize });
    }
    return blocks;
  } catch {
    return null;
  }
}

const sys = {
  pickFolder: async title => (await dialog.showOpenDialog(win, { title, properties: ['openDirectory', 'createDirectory'] })).filePaths[0],
  pickFile: async title => (await dialog.showOpenDialog(win, { title, properties: ['openFile', 'showHiddenFiles'] })).filePaths[0],
  openPath: p => shell.openPath(p),
  showItem: p => shell.showItemInFolder(p),
  openUrl: u => /^https?:/.test(u) && shell.openExternal(u),
  copy: t => clipboard.writeText(t),
  openTerminal: dir => spawn('cmd.exe', ['/c', 'start', 'wt.exe', '-d', dir], { detached: true, windowsHide: true })
    .on('error', () => spawn('cmd.exe', ['/c', 'start', 'cmd.exe'], { cwd: dir, detached: true })),
  openEditor: dir => spawn(vault.settings().editor || 'code', [dir], { shell: true, detached: true, windowsHide: true }),
  readFileText: p => fs.readFileSync(p, 'utf8'),
  defaultCloneDir: () => vault.settings().cloneDir || path.join(app.getPath('documents'), 'GitHub'),
  sshAvailable: () => ssh.available(),

  // Import repos from GitHub Desktop's local state (IndexedDB LevelDB with Snappy decompression, config, or logs).
  githubDesktopRepos: () => {
    const baseDirs = [
      path.join(app.getPath('appData'), 'GitHub Desktop'),
      path.join(app.getPath('home'), '.config', 'GitHub Desktop'),
      path.join(app.getPath('home'), 'Library', 'Application Support', 'GitHub Desktop'),
    ];
    const found = new Set();

    const extractPaths = (text) => {
      if (!text) return;
      const winMatches = text.match(/[a-zA-Z]:\\[^\x00-\x1f"*?<>|;]+/g) || [];
      const unixMatches = text.match(/\/(?:Users|home|[a-zA-Z0-9._-]+)\/[^\x00-\x1f"*?<>|;]+/g) || [];
      for (const p of [...winMatches, ...unixMatches]) {
        let clean = p.trim().replace(/[\x00-\x1f].*$/, '').replace(/[\\/]$/, '');
        if (clean.endsWith('.git')) clean = path.dirname(clean);
        try {
          if (fs.existsSync(clean) && (fs.existsSync(path.join(clean, '.git')) || fs.existsSync(clean + '/.git'))) {
            found.add(path.resolve(clean));
          }
        } catch {}
      }
    };

    for (const base of baseDirs) {
      if (!fs.existsSync(base)) continue;

      // 1. Check repositories.json if present
      const jsonFile = path.join(base, 'repositories.json');
      if (fs.existsSync(jsonFile)) {
        try {
          const raw = JSON.parse(fs.readFileSync(jsonFile, 'utf8'));
          (Array.isArray(raw) ? raw : []).forEach(r => {
            const p = typeof r === 'string' ? r : r && r.path;
            if (p && fs.existsSync(p)) found.add(path.resolve(p));
          });
        } catch {}
      }

      // 2. Parse modern GitHub Desktop IndexedDB LevelDB files with Snappy decompression
      const idbDir = path.join(base, 'IndexedDB', 'file__0.indexeddb.leveldb');
      if (fs.existsSync(idbDir)) {
        try {
          const files = fs.readdirSync(idbDir);
          for (const f of files) {
            if (!f.endsWith('.ldb') && !f.endsWith('.log')) continue;
            try {
              const buf = fs.readFileSync(path.join(idbDir, f));
              const blocks = parseLevelDbBlocks(buf);
              if (blocks && blocks.length > 0) {
                for (const b of blocks) {
                  const type = buf[b.offset + b.size];
                  const raw = buf.subarray(b.offset, b.offset + b.size);
                  const data = type === 1 ? decompressSnappy(raw) : raw;
                  if (data) {
                    extractPaths(data.toString('latin1'));
                    extractPaths(data.toString('utf16le'));
                  }
                }
              } else {
                extractPaths(buf.toString('latin1'));
                extractPaths(buf.toString('utf16le'));
              }
            } catch {}
          }
        } catch {}
      }

      // 3. Parse GitHub Desktop log files
      const logDir = path.join(base, 'logs');
      if (fs.existsSync(logDir)) {
        try {
          const logFiles = fs.readdirSync(logDir);
          for (const lf of logFiles) {
            if (lf.endsWith('.log')) {
              try {
                const content = fs.readFileSync(path.join(logDir, lf), 'utf8');
                extractPaths(content);
              } catch {}
            }
          }
        } catch {}
      }
    }

    return Array.from(found);
  },

  // Scan ~/.ssh (or a specified directory) for SSH keys.
  localSshKeys: (customDir) => {
    const sshDir = customDir || path.join(require('os').homedir(), '.ssh');
    if (!fs.existsSync(sshDir)) return [];
    const files = fs.readdirSync(sshDir);
    const keys = [];
    const seen = new Set();

    // 1. Keys with .pub files
    for (const f of files.filter(f => f.endsWith('.pub'))) {
      const baseName = f.slice(0, -4);
      seen.add(baseName);
      const privPath = path.join(sshDir, baseName);
      const pubPath = path.join(sshDir, f);
      try {
        const content = fs.readFileSync(pubPath, 'utf8').trim();
        const [type, , ...comment] = content.split(' ');
        keys.push({ name: baseName, path: privPath, pubPath, hasPrivate: fs.existsSync(privPath), type, comment: comment.join(' ') });
      } catch {}
    }

    // 2. Private keys without .pub
    for (const f of files) {
      if (seen.has(f) || f.endsWith('.pub') || f.startsWith('known_hosts') || f.startsWith('config')) continue;
      const privPath = path.join(sshDir, f);
      try {
        const stat = fs.statSync(privPath);
        if (!stat.isFile()) continue;
        const head = fs.readFileSync(privPath, 'utf8').slice(0, 100);
        if (head.includes('BEGIN') && head.includes('PRIVATE KEY')) {
          keys.push({ name: f, path: privPath, pubPath: null, hasPrivate: true, type: 'private key', comment: '' });
        }
      } catch {}
    }

    return keys;
  },
};

const vaultOps = {
  listAccounts: vault.listAccounts, saveAccount: vault.saveAccount, removeAccount: vault.removeAccount,
  revealToken: vault.revealToken,
  listSecrets: vault.listSecrets, saveSecret: vault.saveSecret, revealSecret: vault.revealSecret, removeSecret: vault.removeSecret,
  settings: vault.settings, saveSettings: vault.saveSettings, presets: () => vault.PRESETS,
};

const sshOps = {
  list: ssh.list, generate: ssh.generate, setComment: ssh.setComment, changePassphrase: ssh.changePassphrase,
  rename: ssh.rename, remove: ssh.remove, import: ssh.import, addToAgent: ssh.addToAgent, agentList: ssh.agentList, test: ssh.test,
};

const namespaces = { git, repo: repoOps, sys, vault: vaultOps, ssh: sshOps, api: providers };

ipcMain.handle('call', async (_e, ns, fn, args) => {
  const target = namespaces[ns];
  if (!target || typeof target[fn] !== 'function') throw new Error(`Unknown call ${ns}.${fn}`);
  return target[fn](...withProgress(args || []));
});
