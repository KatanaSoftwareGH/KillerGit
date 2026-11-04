# KillerGit

A lightweight Git desktop client for Windows, built with Electron and plain JavaScript (no frontend framework, no bundler).

## Run

Needs [Node.js](https://nodejs.org) (LTS) and Git for Windows.

```
npm install
npm start
```

Build an installer with `npm run dist`.

## Features

**GitHub Desktop baseline**
- Clone (from URL or from your account's repo list), add a local repo, create a new repo (README, .gitignore, license)
- Changes: per-file and per-hunk stage/unstage, discard file or hunk, ignore files, commit summary + description, amend, undo last commit
- History with commit details and diffs; cherry-pick, revert, reset (soft/mixed/hard), tag, branch from a commit
- Branches: create, switch (stash or bring changes along), rename, delete, merge, squash-merge, rebase, check out remote branches, delete remote branches
- Fetch / pull / push / force-push with lease / publish branch; stashes; merge/rebase conflict banner with continue/abort
- Open in Explorer, terminal (Windows Terminal) or editor

**Additions**
- Commit node graph across all branches
- Sidebar grouped by remote host, then namespace; sorted A–Z or by last updated
- Windows 11 native OpenSSH (`C:\Windows\System32\OpenSSH`): generate, import, rename, re-comment, change passphrase, delete, copy public keys, add to ssh-agent, upload to your account
- Accounts for GitHub.com, GitLab.com, Gitgud.io, or any self-hosted GitLab. Tokens are encrypted with Windows DPAPI via Electron `safeStorage`
- Vault for extra tokens, passphrases and notes
- Create repositories on any configured host, from a local repo (adds the remote and pushes) or by itself (then clones)
- Multi-remote: pick a default remote per repo, pull from or push to any remote, push to all remotes at once
- Issues: list, filter, read, comment, open/close, create (GitHub and GitLab APIs)
- Slow connections: chunked push (N commits per push), chunked pull (deepen in steps for shallow clones, bounded fetch otherwise), automatic retries, shallow clone, commit staged files in chunks
- Light, dark or system theme with a choice of accent colors

## Shortcuts
Ctrl+1 Changes · Ctrl+2 History · Ctrl+B branches · Ctrl+Shift+P sync · Ctrl+O add local · Ctrl+N new repo · Ctrl+Shift+R refresh
