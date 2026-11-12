# Builds KillerGit. Usage: powershell -File native\build.ps1 [-Target Windows|Linux|Both] [-Config Release|Debug]
#   Windows: LLVM-MinGW (Clang) + CMake  -> dist\windows\KillerGit.exe   (Direct3D 11)
#   Linux:   GCC + CMake inside WSL      -> dist\linux\killergit + dist\killergit-linux-x86_64.tar.gz   (Vulkan, Wayland + X11)
# Run autosetup.ps1 first to install every toolchain and library.
param(
  [ValidateSet('Windows', 'Linux', 'Both')][string]$Target = 'Windows',
  [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
  [string]$Distro = $env:KILLERGIT_WSL_DISTRO
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here

function Build-Windows {
  Write-Host "=== Windows ($Config) ===" -ForegroundColor Cyan
  # Locate the toolchain: PATH first, then winget's install folder.
  $clang = $null
  $pkg = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'MartinStorsjo.LLVM-MinGW*' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($pkg) { $clang = Get-ChildItem $pkg.FullName -Recurse -Filter clang++.exe | Select-Object -First 1 -ExpandProperty FullName }
  if (-not $clang) {
    # A clang++ on PATH only counts if it is a MinGW one (has windres and mingw32-make next to it).
    $c = (Get-Command clang++.exe -ErrorAction SilentlyContinue).Source
    if ($c -and (Test-Path (Join-Path (Split-Path $c) 'mingw32-make.exe'))) { $clang = $c }
  }
  if (-not $clang) { throw 'LLVM-MinGW not found. Run autosetup.ps1 (or: winget install MartinStorsjo.LLVM-MinGW.UCRT)' }
  $bin = Split-Path $clang
  $env:PATH = "$bin;$env:PATH"
  $bin = $bin -replace '\\', '/'   # CMake treats backslashes as escapes

  $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
  if (-not $cmake) { $cmake = "$env:ProgramFiles\CMake\bin\cmake.exe" }
  $build = Join-Path $here "build\windows-$($Config.ToLower())"
  & $cmake -S $here -B $build -G 'MinGW Makefiles' "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_C_COMPILER=$bin/clang.exe" "-DCMAKE_CXX_COMPILER=$bin/clang++.exe" "-DCMAKE_RC_COMPILER=$bin/llvm-windres.exe" `
    "-DCMAKE_MAKE_PROGRAM=$bin/mingw32-make.exe"
  if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
  & $cmake --build $build -j $env:NUMBER_OF_PROCESSORS
  if ($LASTEXITCODE) { throw "Windows build failed ($LASTEXITCODE)" }
  $out = Join-Path $root 'dist\windows'
  New-Item -ItemType Directory -Force $out | Out-Null
  Copy-Item (Join-Path $build 'KillerGit.exe') $out -Force
  Write-Host "Built $out\KillerGit.exe" -ForegroundColor Green
}

function Get-Distro {
  if ($Distro) { return $Distro }
  # Prefer an Ubuntu/Debian distro; `wsl -l -q` prints UTF-16, so strip the NULs.
  $list = (wsl.exe -l -q) -replace "`0", '' | Where-Object { $_ -and $_ -notmatch '^docker-desktop' }
  $pick = $list | Where-Object { $_ -match 'Ubuntu|Debian' } | Select-Object -First 1
  if (-not $pick) { $pick = $list | Select-Object -First 1 }
  if (-not $pick) { throw 'No WSL distro found. Run autosetup.ps1 to install Ubuntu in WSL.' }
  return $pick.Trim()
}

function Build-Linux {
  Write-Host "=== Linux ($Config) ===" -ForegroundColor Cyan
  if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { throw 'WSL is not installed. Run autosetup.ps1.' }
  $d = Get-Distro
  # Z:\path -> /mnt/z/path
  $p = $here -replace '\\', '/'
  if ($p -match '^([A-Za-z]):(.*)$') { $p = '/mnt/' + $Matches[1].ToLower() + $Matches[2] }
  # Scripts checked out with CRLF won't run in bash: normalise in a temp copy-free way.
  wsl.exe -d $d -- bash -c "sed -i 's/\r$//' '$p/build-linux.sh' '$p/res/linux/install.sh' && bash '$p/build-linux.sh' $Config"
  if ($LASTEXITCODE) { throw "Linux build failed ($LASTEXITCODE)" }
  Write-Host "Built $root\dist\linux\killergit" -ForegroundColor Green
}

switch ($Target) {
  'Windows' { Build-Windows }
  'Linux'   { Build-Linux }
  'Both'    { Build-Windows; Build-Linux }
}
