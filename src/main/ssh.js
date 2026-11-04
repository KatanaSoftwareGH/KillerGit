// SSH key management using Windows 11's built-in OpenSSH (C:\Windows\System32\OpenSSH).
const { execFile } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const DIR = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'OpenSSH');
const exe = name => {
  const p = path.join(DIR, name + '.exe');
  return fs.existsSync(p) ? p : name;
};
const SSH_EXE = exe('ssh');
const SSH_DIR = path.join(os.homedir(), '.ssh');

const runExe = (file, args) => new Promise((resolve, reject) =>
  execFile(file, args, { windowsHide: true }, (err, stdout, stderr) =>
    err ? reject(new Error((stderr || err.message).trim())) : resolve((stdout + stderr).trim())));

async function list() {
  if (!fs.existsSync(SSH_DIR)) return [];
  const out = [];
  for (const f of fs.readdirSync(SSH_DIR).filter(f => f.endsWith('.pub'))) {
    const pub = path.join(SSH_DIR, f);
    const priv = pub.slice(0, -4);
    const content = fs.readFileSync(pub, 'utf8').trim();
    const [type, , ...comment] = content.split(' ');
    let fingerprint = '';
    try { fingerprint = (await runExe(exe('ssh-keygen'), ['-lf', pub])).split(' ')[1]; } catch {}
    out.push({ name: f.slice(0, -4), path: priv, pubPath: pub, hasPrivate: fs.existsSync(priv), type, comment: comment.join(' '), fingerprint, publicKey: content });
  }
  return out;
}

module.exports = {
  SSH_EXE,
  SSH_DIR,
  list,
  available: () => fs.existsSync(path.join(DIR, 'ssh.exe')),
  async generate({ name, type = 'ed25519', comment = '', passphrase = '', bits }) {
    if (!/^[\w.-]+$/.test(name)) throw new Error('Key name may only contain letters, digits, . _ -');
    fs.mkdirSync(SSH_DIR, { recursive: true });
    const p = path.join(SSH_DIR, name);
    if (fs.existsSync(p)) throw new Error('A key with that name already exists');
    const args = ['-t', type, '-f', p, '-N', passphrase, '-C', comment, '-q'];
    if (type === 'rsa') args.push('-b', String(bits || 4096));
    await runExe(exe('ssh-keygen'), args);
    return p;
  },
  setComment: (keyPath, comment, passphrase = '') => runExe(exe('ssh-keygen'), ['-c', '-C', comment, '-P', passphrase, '-f', keyPath]),
  changePassphrase: (keyPath, oldP, newP) => runExe(exe('ssh-keygen'), ['-p', '-P', oldP, '-N', newP, '-f', keyPath]),
  rename(keyPath, newName) {
    const np = path.join(SSH_DIR, newName);
    if (fs.existsSync(np)) throw new Error('Name already in use');
    fs.renameSync(keyPath, np);
    if (fs.existsSync(keyPath + '.pub')) fs.renameSync(keyPath + '.pub', np + '.pub');
    return np;
  },
  remove(keyPath) {
    [keyPath, keyPath + '.pub'].forEach(f => fs.existsSync(f) && fs.unlinkSync(f));
  },
  import(name, privateKey) {
    const p = path.join(SSH_DIR, name);
    fs.mkdirSync(SSH_DIR, { recursive: true });
    fs.writeFileSync(p, privateKey.replace(/\r\n/g, '\n').trim() + '\n', { mode: 0o600 });
    return runExe(exe('ssh-keygen'), ['-y', '-f', p]).then(pub => fs.writeFileSync(p + '.pub', pub + ` ${name}\n`));
  },
  addToAgent: keyPath => runExe(exe('ssh-add'), [keyPath]),
  agentList: () => runExe(exe('ssh-add'), ['-l']).catch(e => e.message),
  test: host => runExe(SSH_EXE, ['-T', '-o', 'StrictHostKeyChecking=accept-new', '-o', 'BatchMode=yes', `git@${host}`]).catch(e => e.message),
};
