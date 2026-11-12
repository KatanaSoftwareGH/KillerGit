# Installs every dependency needed to build KillerGit for Windows and Linux, or updates it if already present.
#   Windows: Git, CMake, LLVM-MinGW (Clang + windres + mingw32-make)      via winget
#   Linux:   WSL + Ubuntu, then GCC, CMake, Vulkan, libcurl, OpenSSL, Wayland/X11 headers   via apt (as root, no password)
# Usage: powershell -ExecutionPolicy Bypass -File autosetup.ps1 [-SkipWindows] [-SkipLinux] [-Distro Ubuntu-24.04]
param(
  [switch]$SkipWindows,
  [switch]$SkipLinux,
  [string]$Distro = $(if ($env:KILLERGIT_WSL_DISTRO) { $env:KILLERGIT_WSL_DISTRO } else { '' })
)
$ErrorActionPreference = 'Stop'
$pathChanged = $false

function Step($t) { Write-Host "`n== $t" -ForegroundColor Cyan }

function Ensure-Winget([string]$id, [string]$name) {
  winget list --id $id -e --accept-source-agreements *> $null
  if ($LASTEXITCODE -eq 0) {
    Write-Host "$name installed, checking for updates..."
    winget upgrade --id $id -e --silent --accept-source-agreements --accept-package-agreements
    # -1978335189 = no applicable update; anything else non-zero is a real failure.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) { Write-Warning "$name upgrade returned $LASTEXITCODE" }
  } else {
    Write-Host "Installing $name..."
    winget install --id $id -e --silent --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "Installing $name failed ($LASTEXITCODE)" }
    $script:pathChanged = $true
  }
}

if (-not $SkipWindows) {
  Step 'Windows toolchain'
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw 'winget is missing. Install "App Installer" from the Microsoft Store, then rerun.' }
  Ensure-Winget 'Git.Git' 'Git'
  Ensure-Winget 'Kitware.CMake' 'CMake'
  Ensure-Winget 'MartinStorsjo.LLVM-MinGW.UCRT' 'LLVM-MinGW'
}

if (-not $SkipLinux) {
  Step 'WSL (Linux builds)'
  $hasWsl = [bool](Get-Command wsl.exe -ErrorAction SilentlyContinue)
  $distros = @()
  if ($hasWsl) { $distros = @((wsl.exe -l -q 2>$null) -replace "`0", '' | Where-Object { $_ -and $_ -notmatch '^docker-desktop' } | ForEach-Object { $_.Trim() }) }
  if (-not $Distro) { $Distro = ($distros | Where-Object { $_ -match 'Ubuntu|Debian' } | Select-Object -First 1) }
  if (-not $Distro) {
    Write-Host 'Installing WSL with Ubuntu 24.04 (needs admin; Windows may ask for a reboot)...'
    wsl.exe --install -d Ubuntu-24.04 --no-launch
    if ($LASTEXITCODE -ne 0) { throw 'WSL install failed. Run this script from an elevated PowerShell, reboot if asked, then rerun.' }
    Write-Warning 'WSL/Ubuntu was just installed. Reboot if Windows asked you to, then rerun autosetup.ps1 to finish the Linux packages.'
    exit 1
  }
  wsl.exe --update 2>$null | Out-Null
  Write-Host "Using WSL distro: $Distro"

  # Build tools + headers. GLFW is fetched by CMake (3.4, Wayland+X11); these are the headers it compiles against.
  # Runtime extras: Vulkan drivers (incl. software lavapipe), zenity for file pickers, xdg-utils, OpenSSH, git.
  $pkgs = @(
    'build-essential', 'cmake', 'pkg-config', 'git', 'curl', 'ca-certificates', 'file',
    'libvulkan-dev', 'mesa-vulkan-drivers', 'vulkan-tools',
    'libcurl4-openssl-dev', 'libssl-dev',
    'libwayland-dev', 'wayland-protocols', 'libxkbcommon-dev', 'libdecor-0-dev',
    'libx11-dev', 'libxrandr-dev', 'libxinerama-dev', 'libxcursor-dev', 'libxi-dev',
    'fontconfig', 'fonts-dejavu-core', 'zenity', 'xdg-utils', 'openssh-client'
  ) -join ' '
  # As root through WSL, so no sudo password prompt. `install` also upgrades packages that are already present.
  $cmd = "export DEBIAN_FRONTEND=noninteractive; apt-get update -q && apt-get install -y -q --no-install-recommends $pkgs && apt-get install -y -q --only-upgrade $pkgs"
  wsl.exe -d $Distro -u root -- bash -c $cmd
  if ($LASTEXITCODE -ne 0) { throw "Installing Linux packages in $Distro failed ($LASTEXITCODE)" }
  [Environment]::SetEnvironmentVariable('KILLERGIT_WSL_DISTRO', $Distro, 'User')
  $env:KILLERGIT_WSL_DISTRO = $Distro
}

Step 'Done'
if ($pathChanged) {
  Write-Host 'New tools were installed. Restart VS Code (or your terminal) so PATH picks them up, then run a build task.' -ForegroundColor Yellow
} else {
  Write-Host 'Everything is installed and up to date. Run "Build" in VS Code (Ctrl+Shift+B).' -ForegroundColor Green
}
