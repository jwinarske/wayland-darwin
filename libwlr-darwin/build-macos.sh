#!/usr/bin/env bash
#
# libwlr-darwin (Cocoa backend) — build.  RUN ON macOS.
#
# Builds the full lower stack (libwayland -> wlroots, via the sibling
# components) into a shared PREFIX, then compiles and links libwlr-darwin and
# the darwin-smoke example against it.
#
# NOTE: this compiles + links only. The darwin-smoke demo opens a real NSWindow
# and needs a windowing session, so it is not run here (CI runners are headless).
set -euo pipefail

export HOMEBREW_NO_REQUIRE_TAP_TRUST=1

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="${SRC:-$ROOT/src}"
PREFIX="${PREFIX:-$HOME/wlroots-darwin/prefix}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

# ---- 1. lower stack: libwayland + wlroots (+ shim, protocols) ---------------
echo "==> building lower stack (wlroots-darwin)"
PREFIX="$PREFIX" SRC="$SRC" "$ROOT/wlroots-darwin/build-macos.sh"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/libdata/pkgconfig:$PREFIX/share/pkgconfig"
for keg in libxkbcommon pixman libffi expat libxml2; do
	p="$(brew --prefix "$keg" 2>/dev/null)/lib/pkgconfig"
	[ -d "$p" ] && PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$p"
done
export PKG_CONFIG_PATH

# ---- 2. libwlr-darwin (Cocoa backend) --------------------------------------
echo "==> building libwlr-darwin"
LWD_BUILD="$HERE/build-macos"
if [ -d "$LWD_BUILD" ]; then
	meson setup "$LWD_BUILD" "$HERE" --prefix "$PREFIX" --reconfigure
else
	meson setup "$LWD_BUILD" "$HERE" --prefix "$PREFIX"
fi
meson compile -C "$LWD_BUILD" -j "$JOBS"
meson install -C "$LWD_BUILD"

echo
echo "==================================================================="
echo " PASS — libwlr-darwin (Cocoa backend) compiles + links on macOS"
echo "        (darwin-smoke built; run it in a login session to see a window)"
echo "==================================================================="
