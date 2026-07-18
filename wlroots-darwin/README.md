# wlroots on macOS (Darwin)

Build support for [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) core
on macOS. The goal is to configure and build the wlroots library on Darwin with
all Linux-graphics backends/renderers disabled, so it can host a native macOS
backend and renderer out of tree.

It builds against the libwayland produced by the sibling **wayland-darwin**
component in this repo (consumed via `PKG_CONFIG_PATH`), and stands up a small **libdrm-compat**
shim so wlroots' pervasive DRM-format vocabulary resolves without a real libdrm
(which does not build on macOS).

Authored against pinned upstream revisions (`pin.env`); compile + runtime happen
on macOS (locally or CI), mirroring the wayland-darwin workflow.

## Source basis (pinned)

| Component | Repo | Commit | Version |
|---|---|---|---|
| wlroots | gitlab.freedesktop.org/wlroots/wlroots `master` | `d64acff` | 0.21.0-dev |
| libdrm | gitlab.freedesktop.org/mesa/drm `main` | `f9816a4` | headers/format-fns only, not built |

## Configure matrix

wlroots' Linux-graphics surface is switched off entirely; only the core library
plus the (portable) pixman renderer path is built:

```
-Dbackends=[] -Drenderers=[] -Dallocators=[] \
-Dsession=disabled -Dxwayland=disabled \
-Dexamples=false -Dtests=false
```

`backends`/`renderers`/`allocators` are array options (empty = none);
`session`/`xwayland` are features (`disabled`). With these, the whole
libinput/libudev/GBM/DRM-backend/Vulkan surface never compiles.

## Core portability blockers (audited against `d64acff`)

| # | Blocker | Location | Resolution |
|---|---|---|---|
| 1 | `cc.find_library('rt')` unconditional | `meson.build:124` | **Fixed** — `required: false` (patch `wlroots/0001`) |
| 2 | `-D_POSIX_C_SOURCE=200809L` unconditional | `meson.build:24` | hides BSD extensions on macOS — exclude Darwin (same fix as wayland-darwin). *Pending.* |
| 3 | libdrm hard dep `>=2.4.129` (subproject fallback won't build on macOS) | `meson.build:100` | **libdrm-compat shim** — vendored headers + real format fns + stubs + a `libdrm.pc` advertising `>=2.4.129`. *In progress (see below).* |
| 4 | `#include <linux/input-event-codes.h>` | `types/wlr_keyboard.c` (only core hit; `backend/x11` is off) | vendor FreeBSD's BSD-2-Clause `input-event-codes.h` behind a compat include dir. *Pending.* |
| 5 | `-Wl,--version-script,wlroots.syms` | `meson.build:150` | GNU-ld only; macOS `ld64` has no `--version-script`. Convert to `-exported_symbols_list` or drop the symbol-visibility script on Darwin. *Pending.* |

The fix approach mirrors wayland-darwin: minimal, individually-upstreamable
patches carried here, applied to a pinned clone by the build script.

## The libdrm-compat shim (design)

Even with every backend/renderer/allocator disabled, wlroots' DRM-format
vocabulary (`wlr_drm_format_set`, fourcc codes, modifiers) is compiled
unconditionally, so the libdrm **headers and a small symbol surface** must be
satisfiable. Real libdrm does not build on macOS (ioctl/`/dev/dri`), so:

- **Headers — vendored from real libdrm, unmodified.** libdrm's `drm.h` already
  has a `#else /* One of the BSDs */` branch using `<stdint.h>` + `<sys/ioccom.h>`
  (both present on macOS), so `drm.h`, `drm_fourcc.h`, `drm_mode.h`, `xf86drm.h`,
  `xf86drmMode.h`, `libdrm_macros.h` compile cleanly on Darwin as-is.
- **Real functions — ported verbatim from `xf86drm.c`:** `drmGetFormatName`,
  `drmGetFormatModifierVendor`, and `drmGetFormatModifierName` (the last via its
  simple-token table; the deep per-vendor modifier decoders are never exercised
  because the Darwin renderer path is LINEAR-only).
- **Behavioral stubs** for the remaining `drm*` symbols wlroots links but never
  reaches without a DRM fd, returning cleanly rather than aborting:
  `drmGetDevices2` → **0** (so renderer autocreate falls to pixman with no error
  spam), `drmFreeDevice` → no-op, device/cap/master queries → `-ENODEV`/`-EINVAL`,
  `drmIoctl`/`drmPrime*`/`drmSyncobj*`/dumb-buffer/`drmMode*` → `-ENOSYS`/NULL.
- **`libdrm.pc`** advertising `Version: 2.4.129` with `Cflags` pointing at the
  vendored headers, so wlroots' `dependency('libdrm', version: '>=2.4.129')`
  resolves via pkg-config and never attempts the subproject fallback.

Referenced `drm*` symbol surface (from source audit) is dominated by
`backend/drm` calls that are **off** in the matrix; the always-linked subset is
small (format fns + a handful of syncobj/dumb/prime stubs). The shim stubs the
full referenced set regardless, to guarantee a one-shot link.

## Status

- [x] Source audit against `d64acff`; all core blockers identified.
- [x] Configure matrix determined.
- [x] wlroots patches (`patches/wlroots/`): librt optional + Darwin meson fixes
  (`_POSIX_C_SOURCE`, `--version-script`), both verified to apply in sequence.
- [x] libdrm-compat shim: `format.c` (real functions, compile+run-verified on
  Linux against the pinned headers) + `stubs.c` (full `drm*` surface, signatures
  verified against the vendored headers). Headers/`.pc`/dylib built by the runbook.
- [x] Vendored FreeBSD `input-event-codes.h` (provided via `-Icompat`, no patch needed).
- [x] `build-macos.sh` (delegates to wayland-darwin for libwayland, builds the
  shim + wayland-protocols, configures wlroots with the matrix) + CI workflow.
- [x] **Green on macOS** — `libwlroots-0.21.dylib` compiles, links, and installs
  on both Apple Silicon runners (macos-14 / macos-15), first try. The shim
  linked with zero missing stubs.
- [x] **Runtime smoke** — `headless-smoke.c` stands up the always-built headless
  backend + pixman renderer + shm allocator, creates a 640x480 output, and
  renders + commits one frame (verified on a native Linux wlroots build; runs on
  macOS against the shim as part of the build). This is the D2 phase-1 software
  path working end to end.

## Next

Enabling a real presented output (a Cocoa/CALayer backend + IOSurface allocator)
and standing up tinywl are the following steps; the core and its software render
path are now proven usable on Darwin.

## Layout

- `pin.env` — pinned wlroots + libdrm + wayland-protocols revisions.
- `patches/wlroots/` — minimal upstreamable wlroots patches (librt, meson Darwin fixes).
- `libdrm-compat/format.c` — real `drmGetFormatName` / modifier helpers.
- `libdrm-compat/stubs.c` — behavioral stubs for the rest of the linked `drm*` surface.
- `compat/linux/input-event-codes.h` — vendored FreeBSD BSD-2-Clause header.
- `headless-smoke.c` — runtime smoke: headless + pixman renders one frame.
- `build-macos.sh` — full build runbook (self-bootstrapping via `pin.env`).
- `../.github/workflows/wlroots-macos.yml` — the CI workflow.
