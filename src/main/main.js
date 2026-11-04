const { app, BrowserWindow, ipcMain, dialog, shell, clipboard, nativeTheme } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');
const git = require('./git');
const ssh = require('./ssh');
const vault = require('./vault');
const providers = require('./providers');

let win;

function createWindow() {
  const s = vault.settings();
  win = new BrowserWindow({
    width: s.width || 1280, height: s.height || 800, minWidth: 900, minHeight: 560,
    backgroundColor: nativeTheme.shouldUseDarkColors ? '#16181d' : '#ffffff',
    autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, spellcheck: true },
  });
  win.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));
  win.on('close', () => { const [width, height] = win.getSize(); vault.saveSettings({ width, height }); });
  win.webContents.setWindowOpenHandler(({ url }) => { shell.openExternal(url); return { action: 'deny' }; });
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());

const progress = text => win && win.webContents.send('progress', text);

// Renderer passes '__progress__' wherever a git op takes an onProgress callback.
const withProgress = args => args.map(a => (a === '__progress__' ? progress : a));

const repoOps = {
  async summary(dir) {
    if (!(await git.isRepo(dir))) return { path: dir, missing: true };
    const remotes = await git.remotes(dir);
    const primary = remotes.find(r => r.name === 'origin') || remotes[0];
    const parsed = primary ? providers.parseRemote(primary.fetch) : null;
    return {
      path: dir, name: path.basename(dir), remotes,
      host: parsed ? parsed.host : null, namespace: parsed ? parsed.namespace : null,
      lastActivity: await git.lastActivity(dir),
    };
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
};

const vaultOps = {
  listAccounts: vault.listAccounts, saveAccount: vault.saveAccount, removeAccount: vault.removeAccount,
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
