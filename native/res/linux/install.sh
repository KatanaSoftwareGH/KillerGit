#!/usr/bin/env sh
# Installs KillerGit for the current user: ~/.local/bin, app menu entry and icon.
# The .desktop file name matches the Wayland app id / X11 WM class, so docks show the right icon.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin"; APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONS="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/512x512/apps"
mkdir -p "$BIN" "$APPS" "$ICONS"
install -m 755 "$HERE/killergit" "$BIN/killergit"
install -m 644 "$HERE/killergit.png" "$ICONS/killergit.png"
sed "s|^Exec=.*|Exec=$BIN/killergit|" "$HERE/killergit.desktop" > "$APPS/killergit.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q "${ICONS%/512x512/apps}" || true
echo "Installed to $BIN/killergit. Runtime needs: git, a Vulkan driver (mesa-vulkan-drivers), libcurl; optional: zenity or kdialog, openssh-client."
