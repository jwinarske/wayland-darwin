#!/usr/bin/env bash
#
# wayland-cxx-scanner (C++17 Wayland bindings + examples) on macOS (Darwin).
#
# Builds the scanner tool and the shm/xdg example clients against the
# wayland-darwin libwayland. The examples that need EGL/GLES/Vulkan/SDL3/Skia
# self-skip (subdir_done) when those deps are absent, so on macOS only the
# CPU/shm examples build. They run against darwin-tinywl (see ../RUNNING.md).
#
# Compiles + links only; running the examples needs a compositor + GUI session.
set -euo pipefail

export HOMEBREW_NO_REQUIRE_TAP_TRUST=1
if ! command -v brew >/dev/null 2>&1; then
	for _b in /opt/homebrew/bin/brew /usr/local/bin/brew; do
		if [ -x "$_b" ]; then eval "$("$_b" shellenv)"; break; fi
	done
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="${SRC:-$ROOT/src}"
PREFIX="${PREFIX:-$HOME/wlroots-darwin/prefix}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

# shellcheck disable=SC1091
. "$HERE/pin.env"
fetch_pin() {  # url ref dest
	local url="$1" ref="$2" dest="$3"
	[ -d "$dest/.git" ] && { echo "    have $(basename "$dest")"; return; }
	echo "==> fetching $(basename "$dest") @ ${ref:0:12}"
	mkdir -p "$dest"; git -C "$dest" init -q
	git -C "$dest" remote add origin "$url" 2>/dev/null || true
	local n=1
	while ! git -C "$dest" fetch -q --depth 1 origin "$ref"; do
		if [ "$n" -ge 5 ]; then echo "    fetch failed after $n attempts" >&2; exit 1; fi
		echo "    fetch failed (attempt $n), retrying in 5s..." >&2; sleep 5; n=$((n + 1))
	done
	git -C "$dest" checkout -q FETCH_HEAD
}

# ---- 1. libwayland + epoll-shim (delegate to the wayland-darwin component) --
echo "==> building libwayland (wayland-darwin)"
PREFIX="$PREFIX" SRC="$SRC" "$ROOT/wayland-darwin/build-macos.sh"

# ---- 2. Homebrew deps + pkg-config path -------------------------------------
brew install cmake ninja meson pkgconf libxkbcommon pugixml >/dev/null || true
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/libdata/pkgconfig:$PREFIX/share/pkgconfig"
for keg in libxkbcommon pugixml libffi expat libxml2; do
	p="$(brew --prefix "$keg" 2>/dev/null)/lib/pkgconfig"
	[ -d "$p" ] && PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$p"
done
export PKG_CONFIG_PATH

# ---- 3. fetch pinned wayland-protocols + wayland-cxx-scanner ----------------
fetch_pin "$WAYLAND_PROTOCOLS_URL" "$WAYLAND_PROTOCOLS_REF" "$SRC/wayland-protocols"
fetch_pin "$WAYLAND_CXX_URL"       "$WAYLAND_CXX_REF"       "$SRC/wayland-cxx-scanner"

# ---- 4. wayland-protocols (installs XML + .pc) ------------------------------
echo "==> installing wayland-protocols"
WP_BUILD="$SRC/wayland-protocols/build-macos"
[ -d "$WP_BUILD" ] && meson setup "$WP_BUILD" "$SRC/wayland-protocols" --prefix "$PREFIX" -Dtests=false --reconfigure \
	|| meson setup "$WP_BUILD" "$SRC/wayland-protocols" --prefix "$PREFIX" -Dtests=false
meson install -C "$WP_BUILD"

# ---- 5. build wayland-cxx-scanner + examples -------------------------------
# Darwin injections (no source patches to the upstream project):
#  - keyboard.hpp/cursor.hpp use <sys/timerfd.h> -> epoll-shim's include + link
#  - some examples include <linux/input-event-codes.h> (vendored in
#    wlroots-darwin/compat) and <sys/sysmacros.h> (shim in ./compat)
#  - werror relaxed (clang on macOS emits warnings gcc/Linux does not)
echo "==> building wayland-cxx-scanner"
WCS="$SRC/wayland-cxx-scanner"
WCS_BUILD="$WCS/build-macos"
COMPAT_INC="-I$PREFIX/include/libepoll-shim -I$ROOT/wlroots-darwin/compat -I$HERE/compat -include $HERE/compat/darwin_syscalls.h"
WCS_OPTS=(
	--prefix "$PREFIX"
	-Dexamples=true -Dtests=false -Dwerror=false
	"-Dcpp_args=$COMPAT_INC"
	"-Dcpp_link_args=-L$PREFIX/lib -lepoll-shim -Wl,-rpath,$PREFIX/lib"
)
if [ -d "$WCS_BUILD" ]; then
	meson setup "$WCS_BUILD" "$WCS" "${WCS_OPTS[@]}" --reconfigure
else
	meson setup "$WCS_BUILD" "$WCS" "${WCS_OPTS[@]}"
fi
meson compile -C "$WCS_BUILD" -j "$JOBS"

echo
echo "==================================================================="
echo " PASS — wayland-cxx-scanner + shm examples build on macOS"
echo "        (GL/Vulkan/SDL3/Skia examples self-skip; run shm ones vs darwin-tinywl)"
echo "==================================================================="
echo "built example binaries:"
find "$WCS_BUILD/examples" -maxdepth 2 -type f -perm -u+x 2>/dev/null \
	| grep -vE '\.(o|p|dylib)$' | sed 's|.*/||' | sort | sed 's/^/  /'
