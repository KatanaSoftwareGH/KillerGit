# KillerGit

A lightweight, native Git desktop client written in C++ with Dear ImGui.

- **Windows**: Win32 + Direct3D 11, a single static `KillerGit.exe`.
- **Linux**: GLFW 3.4 + Vulkan. One binary runs natively on **Wayland** and on **X11** (backend picked at runtime; force with `KILLERGIT_PLATFORM=wayland|x11`). HiDPI and fractional scaling render at full resolution.

## Build

Run once (and again to update everything):

```
powershell -ExecutionPolicy Bypass -File autosetup.ps1
```

It installs or updates Git, CMake and LLVM-MinGW (winget) for Windows, and WSL + Ubuntu with GCC, Vulkan, libcurl, OpenSSL and the Wayland/X11 headers for Linux.

Then in VS Code press **Ctrl+Shift+B** → **Build** and pick **Windows**, **Linux** or **Both**. From a terminal:

```
powershell -File nativeuild.ps1 -Target Both
```

Output:
- `dist/windows/KillerGit.exe`
- `dist/linux/killergit` plus `dist/killergit-linux-x86_64.tar.gz` (run `install.sh` inside it to add the app to your menu)

Linux runtime needs `git`, a Vulkan driver (`mesa-vulkan-drivers` or your GPU vendor's) and `libcurl`. `zenity` or `kdialog` provides the folder pickers. On a Linux host you can build directly with `native/build-linux.sh`.

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
