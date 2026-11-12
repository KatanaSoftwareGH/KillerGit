#!/usr/bin/env node
// Minimal read-only MCP server (stdio, newline-delimited JSON-RPC) exposing KillerGit-style git views.
// No dependencies; shells out to the git CLI. Every tool takes `repo` (path to a working tree).
const { execFile } = require('child_process');

const SEP = '\x1f', REC = '\x1e';

function git(cwd, args) {
  return new Promise((resolve, reject) => {
    execFile('git', args, { cwd, maxBuffer: 64 * 1024 * 1024, windowsHide: true,
      env: { ...process.env, GIT_TERMINAL_PROMPT: '0', LC_ALL: 'C' } },
      (err, out, errOut) => err ? reject(new Error(errOut.trim() || err.message)) : resolve(out));
  });
}

const repoProp = { repo: { type: 'string', description: 'Absolute path to the git repository' } };
const tools = {
  git_status: {
    description: 'Branch, upstream, ahead/behind and changed files (porcelain status).',
    props: {},
    run: ({ repo }) => git(repo, ['status', '--porcelain=v1', '-b', '-uall']),
  },
  git_log: {
    description: 'Commit graph data: hash, parents, author, date, refs, subject.',
    props: { limit: { type: 'number', description: 'Max commits (default 50)' }, all: { type: 'boolean', description: 'Include all branches (default true)' } },
    run: async ({ repo, limit = 50, all = true }) => {
      const fmt = ['%H', '%P', '%an', '%aI', '%D', '%s'].join(SEP) + REC;
      const out = await git(repo, ['log', ...(all ? ['--all'] : []), '--topo-order', `-n${Math.min(limit, 1000)}`, `--format=${fmt}`]);
      return out.split(REC).map(s => s.trim()).filter(Boolean).map(r => {
        const [hash, parents, author, date, refs, subject] = r.split(SEP);
        return { hash, parents: parents ? parents.split(' ') : [], author, date, refs: refs ? refs.split(', ') : [], subject };
      });
    },
  },
  git_diff: {
    description: 'Working-tree or staged diff, optionally for one file.',
    props: { file: { type: 'string' }, staged: { type: 'boolean' } },
    run: ({ repo, file, staged }) => git(repo, ['diff', ...(staged ? ['--cached'] : []), ...(file ? ['--', file] : [])]),
  },
  git_show: {
    description: 'Show a commit (message and patch).',
    props: { hash: { type: 'string' } },
    required: ['hash'],
    run: ({ repo, hash }) => git(repo, ['show', '--stat', '--patch', hash]),
  },
  git_branches: {
    description: 'Local branches, remote branches and tags with upstream tracking info.',
    props: {},
    run: ({ repo }) => git(repo, ['for-each-ref', '--format=%(HEAD) %(refname:short) %(upstream:short) %(upstream:track)', 'refs/heads', 'refs/remotes', 'refs/tags']),
  },
  git_remotes: {
    description: 'Configured remotes and URLs.',
    props: {},
    run: ({ repo }) => git(repo, ['remote', '-v']),
  },
  git_stashes: {
    description: 'List stashes.',
    props: {},
    run: ({ repo }) => git(repo, ['stash', 'list']),
  },
};

const listTools = () => Object.entries(tools).map(([name, t]) => ({
  name, description: t.description,
  inputSchema: { type: 'object', properties: { ...repoProp, ...t.props }, required: ['repo', ...(t.required || [])] },
  annotations: { readOnlyHint: true },
}));

async function handle(msg) {
  switch (msg.method) {
    case 'initialize':
      return { protocolVersion: msg.params?.protocolVersion || '2024-11-05', capabilities: { tools: {} },
        serverInfo: { name: 'killergit', version: '0.1.0' } };
    case 'tools/list': return { tools: listTools() };
    case 'tools/call': {
      const t = tools[msg.params.name];
      if (!t) throw Object.assign(new Error('Unknown tool'), { code: -32602 });
      try {
        const r = await t.run(msg.params.arguments || {});
        return { content: [{ type: 'text', text: typeof r === 'string' ? r : JSON.stringify(r, null, 2) }] };
      } catch (e) { return { isError: true, content: [{ type: 'text', text: e.message }] }; }
    }
    case 'ping': return {};
    default:
      if (msg.id === undefined) return undefined; // notification
      throw Object.assign(new Error('Method not found'), { code: -32601 });
  }
}

function serve() {
let buf = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', chunk => {
  buf += chunk;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch { continue; }
    handle(msg).then(result => {
      if (msg.id !== undefined && result !== undefined) process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id: msg.id, result }) + '\n');
    }).catch(e => {
      if (msg.id !== undefined) process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id: msg.id, error: { code: e.code || -32603, message: e.message } }) + '\n');
    });
  }
});
}

module.exports = { tools, serve };
if (require.main === module) serve();
