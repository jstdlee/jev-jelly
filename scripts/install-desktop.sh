#!/usr/bin/env bash
# Adds a "Jelly" launcher with the jelly icon to the app menu and the desktop (GNOME / KDE, no root needed).
#   scripts/install-desktop.sh            install
#   scripts/install-desktop.sh --remove   take it away again
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
icons="$HOME/.local/share/icons/hicolor/scalable/apps"
apps="$HOME/.local/share/applications"
desk="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")"
if [ "${1:-}" = "--remove" ]; then
  rm -f "$icons/jev-jelly.svg" "$apps/jev-jelly.desktop" "$desk/jev-jelly.desktop"
  echo "removed"; exit 0
fi
[ -x "$root/jelly" ] || { echo "build it first: make" >&2; exit 1; }
mkdir -p "$icons" "$apps" "$desk"
cp "$root/assets/jev-jelly.svg" "$icons/jev-jelly.svg"
cat > "$apps/jev-jelly.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Jelly
GenericName=Desktop jelly friend
Comment=A squishy jelly friend that lives on your desktop
Exec=$root/scripts/run-jelly.sh
Path=$root
Icon=jev-jelly
Terminal=false
Categories=Amusement;Utility;
Keywords=jelly;pet;desktop;toy;chat;
StartupNotify=false
DESK
chmod +x "$apps/jev-jelly.desktop" "$root/scripts/run-jelly.sh"
cp "$apps/jev-jelly.desktop" "$desk/jev-jelly.desktop"
chmod +x "$desk/jev-jelly.desktop"
# the GNOME desktop icons extension only launches files it trusts
gio set "$desk/jev-jelly.desktop" metadata::trusted true 2>/dev/null || true
gtk-update-icon-cache -q -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
update-desktop-database -q "$apps" 2>/dev/null || true
echo "installed: app menu + $desk/jev-jelly.desktop"
