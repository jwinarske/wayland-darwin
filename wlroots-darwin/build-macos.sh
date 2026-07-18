#!/usr/bin/env bash
#
# wlroots core on macOS (Darwin).  RUN ON macOS.
#
# Builds the wlroots library on Darwin with all Linux-graphics backends,
# renderers, and allocators disabled, against:
#   - the libwayland from the sibling wayland-darwin component (delegated to),
#   - a libdrm-compat shim (vendored headers + format fns + stubs), and
#   - Homebrew xkbcommon/pixman + a pinned wayland-protocols.
# Everything installs into a private PREFIX; nothing touches the system.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"                 # repo root: wayland-darwin/ + wlroots-darwin/
SRC="${SRC:-$ROOT/src}"
PREFIX="${PREFIX:-$HOME/wlroots-darwin/prefix}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
echo "==> SRC=$SRC  PREFIX=$PREFIX  JOBS=$JOBS"

# shellcheck disable=SC1091
. "$HERE/pin.env"
fetch_pin() {  # url ref dest
	local url="$1" ref="$2" dest="$3"
	[ -d "$dest/.git" ] && { echo "    have $(basename "$dest")"; return; }
	echo "==> fetching $(basename "$dest") @ ${ref:0:12}"
	mkdir -p "$dest"; git -C "$dest" init -q
	git -C "$dest" remote add origin "$url" 2>/dev/null || true
	git -C "$dest" fetch -q --depth 1 origin "$ref"
	git -C "$dest" checkout -q FETCH_HEAD
}
apply_patches() {  # dest patchdir
	local dest="$1" dir="$2" p
	for p in "$dir"/*.patch; do
		[ -e "$p" ] || continue
		if git -C "$dest" apply --reverse --check "$p" 2>/dev/null; then
			echo "    already applied: $(basename "$p")"
		else
			git -C "$dest" apply "$p"; echo "    applied: $(basename "$p")"
		fi
	done
}

# ---- 1. libwayland + epoll-shim + wayland-scanner ---------------------------
# Delegate to the sibling component; it installs into the shared PREFIX.
echo "==> building libwayland (wayland-darwin)"
PREFIX="$PREFIX" SRC="$SRC" "$ROOT/wayland-darwin/build-macos.sh"

# ---- 2. Homebrew deps + pkg-config path -------------------------------------
brew install cmake meson ninja pkgconf libxkbcommon pixman >/dev/null || true
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/libdata/pkgconfig:$PREFIX/share/pkgconfig"
for keg in libxkbcommon pixman libffi expat libxml2; do
	p="$(brew --prefix "$keg" 2>/dev/null)/lib/pkgconfig"
	[ -d "$p" ] && PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$p"
done
export PKG_CONFIG_PATH
echo "==> PKG_CONFIG_PATH=$PKG_CONFIG_PATH"

# ---- 3. fetch pinned wlroots / libdrm / wayland-protocols -------------------
fetch_pin "$WLROOTS_URL"            "$WLROOTS_REF"            "$SRC/wlroots"
fetch_pin "$LIBDRM_URL"             "$LIBDRM_REF"             "$SRC/libdrm"
fetch_pin "$WAYLAND_PROTOCOLS_URL"  "$WAYLAND_PROTOCOLS_REF"  "$SRC/wayland-protocols"

# ---- 4. wayland-protocols (installs XML + .pc) ------------------------------
echo "==> installing wayland-protocols"
WP_BUILD="$SRC/wayland-protocols/build-macos"
[ -d "$WP_BUILD" ] && meson setup "$WP_BUILD" "$SRC/wayland-protocols" --prefix "$PREFIX" -Dtests=false --reconfigure \
	|| meson setup "$WP_BUILD" "$SRC/wayland-protocols" --prefix "$PREFIX" -Dtests=false
meson install -C "$WP_BUILD"

# ---- 5. libdrm-compat shim: vendored headers + dylib + libdrm.pc ------------
# Real libdrm does not build on macOS; wlroots only needs the DRM format
# vocabulary (headers) plus a small symbol surface it never reaches without a
# DRM fd. Vendor libdrm's (BSD-portable) headers and build the shim.
echo "==> building libdrm-compat shim"
mkdir -p "$PREFIX/include/libdrm" "$PREFIX/lib/pkgconfig"
cp "$SRC/libdrm/xf86drm.h" "$SRC/libdrm/xf86drmMode.h" "$PREFIX/include/libdrm/"
cp "$SRC/libdrm/include/drm/drm.h" \
   "$SRC/libdrm/include/drm/drm_fourcc.h" \
   "$SRC/libdrm/include/drm/drm_mode.h" "$PREFIX/include/libdrm/"
cc -dynamiclib -O2 -fPIC -I"$PREFIX/include/libdrm" \
	-install_name "$PREFIX/lib/libdrm.dylib" \
	"$HERE/libdrm-compat/format.c" "$HERE/libdrm-compat/stubs.c" \
	-o "$PREFIX/lib/libdrm.dylib"
cat > "$PREFIX/lib/pkgconfig/libdrm.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libdrm
Description: libdrm-compat shim for Darwin (headers + format fns + stubs)
Version: 2.4.129
Libs: -L\${libdir} -ldrm
Cflags: -I\${includedir}/libdrm
EOF
pkg-config --exists 'libdrm >= 2.4.129' && echo "    libdrm.pc OK ($(pkg-config --modversion libdrm))"

# ---- 6. apply wlroots Darwin patches ---------------------------------------
echo "==> applying wlroots Darwin patches"
apply_patches "$SRC/wlroots" "$HERE/patches/wlroots"

# ---- 7. configure + build wlroots (core only) ------------------------------
# All Linux-graphics surface off; werror off for the Darwin port; the vendored
# FreeBSD input-event-codes.h is provided via -Icompat.
echo "==> building wlroots"
WLR_BUILD="$SRC/wlroots/build-macos"
WLR_OPTS=(
	--prefix "$PREFIX"
	-Dbackends=[] -Drenderers=[] -Dallocators=[]
	-Dsession=disabled -Dxwayland=disabled
	-Dexamples=false -Dtests=false -Dwerror=false
	"-Dc_args=-I$HERE/compat"
)
if [ -d "$WLR_BUILD" ]; then
	meson setup "$WLR_BUILD" "$SRC/wlroots" "${WLR_OPTS[@]}" --reconfigure
else
	meson setup "$WLR_BUILD" "$SRC/wlroots" "${WLR_OPTS[@]}"
fi
meson compile -C "$WLR_BUILD" -j "$JOBS"
meson install -C "$WLR_BUILD"

echo
echo "==================================================================="
echo " PASS — wlroots core builds and installs on macOS"
echo "==================================================================="
