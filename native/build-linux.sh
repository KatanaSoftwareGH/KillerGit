#!/usr/bin/env bash
# Builds the Linux binary (GLFW Wayland+X11, Vulkan) and stages a portable bundle in dist/linux.
# Usage: ./build-linux.sh [Release|Debug] [build-dir]
# On Windows this runs inside WSL (see build.ps1 -Target Linux); the build dir defaults to the Linux
# filesystem because compiling on /mnt/<drive> is several times slower.
set -euo pipefail
CONFIG="${1:-Release}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${2:-$HOME/.cache/killergit-build/$CONFIG}"
OUT="$HERE/../dist/linux"

cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD" -j"$(nproc)"

rm -rf "$OUT"; mkdir -p "$OUT"
cp "$BUILD/KillerGit" "$OUT/killergit"
[ "$CONFIG" = "Release" ] && strip "$OUT/killergit"
cp "$HERE/res/icon-512.png" "$OUT/killergit.png"
cp "$HERE/res/linux/killergit.desktop" "$HERE/res/linux/install.sh" "$OUT/"
chmod +x "$OUT/killergit" "$OUT/install.sh"
( cd "$OUT/.." && tar -czf killergit-linux-x86_64.tar.gz linux )
echo "Built $OUT/killergit (bundle: dist/killergit-linux-x86_64.tar.gz)"
