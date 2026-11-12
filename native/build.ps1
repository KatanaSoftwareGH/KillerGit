# Builds KillerGit.exe with LLVM-MinGW (Clang) + CMake. Usage: powershell -File native\build.ps1 [-Config Debug]
param([string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Locate the toolchain: PATH first, then winget's install folder.
$clang = (Get-Command clang++.exe -ErrorAction SilentlyContinue).Source
if (-not $clang) {
  $pkg = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory -Filter 'MartinStorsjo.LLVM-MinGW*' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($pkg) { $clang = Get-ChildItem $pkg.FullName -Recurse -Filter clang++.exe | Select-Object -First 1 -ExpandProperty FullName }
}
if (-not $clang) { throw 'LLVM-MinGW not found. Install: winget install MartinStorsjo.LLVM-MinGW.UCRT' }
$bin = Split-Path $clang
$env:PATH = "$bin;$env:PATH"
$bin = $bin -replace '\\', '/'   # CMake treats backslashes as escapes

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "$env:ProgramFiles\CMake\bin\cmake.exe" }
$build = Join-Path $here 'build'
& $cmake -S $here -B $build -G 'MinGW Makefiles' "-DCMAKE_BUILD_TYPE=$Config" `
  "-DCMAKE_C_COMPILER=$bin/clang.exe" "-DCMAKE_CXX_COMPILER=$bin/clang++.exe" "-DCMAKE_RC_COMPILER=$bin/llvm-windres.exe" `
  "-DCMAKE_MAKE_PROGRAM=$bin/mingw32-make.exe"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build $build -j $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "Built $build\KillerGit.exe"
