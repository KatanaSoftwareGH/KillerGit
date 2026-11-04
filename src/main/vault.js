// Encrypted store for accounts (tokens, assigned SSH keys) and app settings.
// Secrets are encrypted with Electron safeStorage (DPAPI on Windows).
const { app, safeStorage } = require('electron');
const fs = require('fs');
const path = require('path');

const file = () => path.join(app.getPath('userData'), 'vault.bin');
const settingsFile = () => path.join(app.getPath('userData'), 'settings.json');

let data = null;

function load() {
  if (data) return data;
  try {
    const raw = fs.readFileSync(file());
    data = JSON.parse(safeStorage.isEncryptionAvailable() ? safeStorage.decryptString(raw) : raw.toString());
  } catch { data = { accounts: [], secrets: [] }; }
  return data;
}

function save() {
  const json = JSON.stringify(data);
  fs.writeFileSync(file(), safeStorage.isEncryptionAvailable() ? safeStorage.encryptString(json) : json);
}

const PRESETS = {
  github: { host: 'github.com', api: 'https://api.github.com' },
  gitlab: { host: 'gitlab.com', api: 'https://gitlab.com/api/v4' },
  gitgud: { host: 'gitgud.io', api: 'https://gitgud.io/api/v4' },
};

module.exports = {
  PRESETS,
  // Accounts are returned without their tokens; the renderer never sees secrets back.
  listAccounts: () => load().accounts.map(({ token, ...a }) => ({ ...a, hasToken: !!token })),
  saveAccount(acct) {
    load();
    const preset = PRESETS[acct.provider] || {};
    const a = {
      id: acct.id || Date.now().toString(36),
      provider: acct.provider,
      kind: acct.provider === 'github' ? 'github' : 'gitlab',
      host: (acct.host || preset.host || '').toLowerCase(),
      api: acct.api || preset.api || `https://${acct.host}/api/v4`,
      username: acct.username || '',
      sshKey: acct.sshKey || '',
      label: acct.label || '',
    };
    const old = data.accounts.find(x => x.id === a.id);
    a.token = acct.token || (old && old.token) || '';
    data.accounts = data.accounts.filter(x => x.id !== a.id).concat(a);
    save();
    return a.id;
  },
  removeAccount(id) { load(); data.accounts = data.accounts.filter(a => a.id !== id); save(); },
  account: id => load().accounts.find(a => a.id === id),
  accountForHost: host => load().accounts.find(a => a.host === host),
  // Free-form secrets (extra tokens, notes, key passphrases).
  listSecrets: () => load().secrets.map(({ value, ...s }) => s),
  saveSecret(s) {
    load();
    const id = s.id || Date.now().toString(36);
    const old = data.secrets.find(x => x.id === id);
    data.secrets = data.secrets.filter(x => x.id !== id).concat({ id, name: s.name, kind: s.kind, value: s.value || (old && old.value) || '' });
    save();
  },
  revealSecret: id => (load().secrets.find(s => s.id === id) || {}).value || '',
  removeSecret(id) { load(); data.secrets = data.secrets.filter(s => s.id !== id); save(); },

  settings() {
    try { return JSON.parse(fs.readFileSync(settingsFile(), 'utf8')); } catch { return {}; }
  },
  saveSettings(s) {
    const merged = { ...module.exports.settings(), ...s };
    fs.writeFileSync(settingsFile(), JSON.stringify(merged, null, 2));
    return merged;
  },
};
