#!/bin/sh
# Builds without root on Debian/Ubuntu: downloads the X11/GL dev packages (no install), extracts their headers into
# third_party/sysroot, and points third_party/lib at the system's runtime libraries.
# With root you can instead run:  sudo apt install libx11-dev libxext-dev libgl-dev
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
(cd "$tmp" && apt-get download libx11-dev libxext-dev x11proto-dev libgl-dev libglx-dev libegl-dev mesa-common-dev)
for d in "$tmp"/*.deb; do dpkg -x "$d" third_party/sysroot; done
rm -rf "$tmp"
libdir=/usr/lib/$(gcc -print-multiarch)
mkdir -p third_party/lib
ln -sf "$libdir/libX11.so.6" third_party/lib/libX11.so
ln -sf "$libdir/libXext.so.6" third_party/lib/libXext.so
ln -sf "$libdir/libGL.so.1" third_party/lib/libGL.so
echo "headers in third_party/sysroot, libs linked in third_party/lib — now run make"
