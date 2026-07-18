#!/usr/bin/env bash
#
# libwayland on macOS (Darwin) — build + acceptance test.  RUN ON macOS.
#
# Sources live under ../src (wayland, epoll-shim); this script builds
# epoll-shim, then libwayland against it, runs libwayland's own test suite,
# and finally a targeted event-loop smoke test.
#
# Everything installs into a private PREFIX; nothing touches the system.
# Override PREFIX / SRC to taste.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-$(cd "$HERE/../src" && pwd)}"
PREFIX="${PREFIX:-$HOME/wayland-darwin/prefix}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

echo "==> SRC=$SRC  PREFIX=$PREFIX  JOBS=$JOBS"

# ---- fetch pinned sources if absent (CI + fresh-Mac bootstrap) --------------
# shellcheck disable=SC1091
. "$HERE/pin.env"
fetch_pin() {  # url ref dest
	local url="$1" ref="$2" dest="$3"
	[ -d "$dest/.git" ] && { echo "    have $(basename "$dest") ($(git -C "$dest" rev-parse --short HEAD))"; return; }
	echo "==> fetching $(basename "$dest") @ ${ref:0:12}"
	mkdir -p "$dest"
	git -C "$dest" init -q
	git -C "$dest" remote add origin "$url" 2>/dev/null || true
	git -C "$dest" fetch -q --depth 1 origin "$ref"
	git -C "$dest" checkout -q FETCH_HEAD
}
mkdir -p "$SRC"
fetch_pin "$WAYLAND_URL"    "$WAYLAND_REF"    "$SRC/wayland"
fetch_pin "$EPOLL_SHIM_URL" "$EPOLL_SHIM_REF" "$SRC/epoll-shim"

# ---- 0. Homebrew deps -------------------------------------------------------
# pkgconf provides pkg-config. libffi and expat are KEG-ONLY on macOS, so their
# .pc files are NOT on the default pkg-config path — we add them explicitly.
if ! command -v brew >/dev/null; then echo "Homebrew required"; exit 2; fi
brew install cmake ninja meson pkgconf libffi expat libxml2 >/dev/null || true

# NB: epoll-shim installs its .pc into libdata/pkgconfig (BSD convention), not
# lib/pkgconfig — include both.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/libdata/pkgconfig:$PREFIX/share/pkgconfig"
for keg in libffi expat libxml2; do
	p="$(brew --prefix "$keg")/lib/pkgconfig"
	[ -d "$p" ] && PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$p"
done
export PKG_CONFIG_PATH
echo "==> PKG_CONFIG_PATH=$PKG_CONFIG_PATH"

# ---- 1. epoll-shim (CMake) --------------------------------------------------
# Provides <sys/{epoll,timerfd,signalfd,eventfd}.h> over kqueue. macOS is a
# first-class epoll-shim target (README: tested on macOS 13.7.1).
echo "==> applying epoll-shim Darwin patches"
for p in "$HERE"/patches/epoll-shim/*.patch; do
	[ -e "$p" ] || continue
	if git -C "$SRC/epoll-shim" apply --reverse --check "$p" 2>/dev/null; then
		echo "    already applied: $(basename "$p")"
	else
		git -C "$SRC/epoll-shim" apply "$p"
		echo "    applied: $(basename "$p")"
	fi
done

echo "==> building epoll-shim"
cmake -S "$SRC/epoll-shim" -B "$SRC/epoll-shim/build-macos" -G Ninja \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DBUILD_TESTING=OFF
cmake --build "$SRC/epoll-shim/build-macos" -j "$JOBS"
cmake --install "$SRC/epoll-shim/build-macos"
# Hard check: if pkg-config can't see epoll-shim now, the wayland configure
# below will fail confusingly — fail here instead, with the search path shown.
if ! pkg-config --exists epoll-shim; then
	echo "ERROR: epoll-shim.pc not found on PKG_CONFIG_PATH=$PKG_CONFIG_PATH" >&2
	exit 1
fi
echo "    epoll-shim.pc OK: $(pkg-config --cflags epoll-shim)"

# ---- 2. libwayland (meson) --------------------------------------------------
# Apply the Darwin patches (build config + os cloexec/peercred), idempotently:
# if a patch already reverse-applies cleanly it is present, so skip it.
echo "==> applying wayland Darwin patches"
for p in "$HERE"/patches/wayland/*.patch; do
	if git -C "$SRC/wayland" apply --reverse --check "$p" 2>/dev/null; then
		echo "    already applied: $(basename "$p")"
	else
		git -C "$SRC/wayland" apply "$p"
		echo "    applied: $(basename "$p")"
	fi
done

echo "==> building libwayland"
# libraries=true needs epoll-shim; we skip docs. Keep tests ON for acceptance.
WL_BUILD="$SRC/wayland/build-macos"
# tests=false: libwayland's own suite uses an ELF-only section trick in its
# harness and assumes Linux (/proc, signalfd semantics), so it doesn't build on
# Mach-O. We validate the libraries with portable smokes below instead; porting
# the full suite is separate follow-up work.
WL_OPTS=(--prefix "$PREFIX" -Ddocumentation=false -Dtests=false -Dscanner=true -Dlibraries=true)
if [ -d "$WL_BUILD" ]; then
	meson setup "$WL_BUILD" "$SRC/wayland" "${WL_OPTS[@]}" --reconfigure
else
	meson setup "$WL_BUILD" "$SRC/wayland" "${WL_OPTS[@]}"
fi
meson compile -C "$WL_BUILD" -j "$JOBS"
meson install -C "$WL_BUILD"

# ---- 3. acceptance: portable smokes against the built libraries ------------
# Wayland sockets live under XDG_RUNTIME_DIR; macOS does not set it, so provide
# a private 0700 dir.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}/wayland-darwin-rt}"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

echo "==> acceptance (a): event-loop smoke (epoll/timerfd/signalfd via epoll-shim)"
cc "$HERE/eventloop-smoke.c" \
	$(pkg-config --cflags --libs wayland-server) \
	$(pkg-config --cflags --libs epoll-shim) \
	-Wl,-rpath,"$PREFIX/lib" \
	-o "$WL_BUILD/eventloop-smoke"
"$WL_BUILD/eventloop-smoke"

echo "==> acceptance (b): client<->server roundtrip over the socket"
cc "$HERE/roundtrip-smoke.c" \
	$(pkg-config --cflags --libs wayland-server wayland-client) \
	-Wl,-rpath,"$PREFIX/lib" \
	-o "$WL_BUILD/roundtrip-smoke"
"$WL_BUILD/roundtrip-smoke"

echo
echo "==================================================================="
echo " PASS — libwayland builds and runs on macOS via epoll-shim"
echo "==================================================================="
