# wayland-cxx-scanner on macOS (Darwin)

Build support for [wayland-cxx-scanner](https://github.com/jwinarske/wayland-cxx-scanner)
— a C++17 Wayland bindings generator and example clients — on macOS, against the
`wayland-darwin` libwayland. Its shm/xdg example clients run against
`darwin-tinywl`.

This builds the upstream project unchanged: the only Darwin-specific wiring is
in `build-macos.sh` (a couple of injected compiler flags), no source patches.

## What builds on macOS

The project gates cleanly, so `-Dexamples=true` on Darwin builds the CPU/shm
examples and **self-skips** the rest:

- **Builds & runs** (against `darwin-tinywl`): `minimal`, `wayland-info`,
  `presentation-shm`, `ivi-presentation-shm`, `agl-presentation-shm`,
  `subsurfaces`, `key-input`, `clipboard`, `ext-data-control`, `text-input`,
  `xdg-ssd`, `xdg-csd`.
- **Self-skip** (no client-side GL/GPU on this stack): everything EGL / GLES /
  Vulkan / dmabuf / SDL3 / Skia / ImGui — each does `subdir_done()` when its
  deps are absent.

## The one Darwin detail

The framework's `keyboard.hpp` / `cursor.hpp` use `<sys/timerfd.h>`
(key-repeat / cursor-animation timers). macOS has no timerfd natively — it comes
from **epoll-shim**, which `wayland-darwin` already builds into the prefix (with
the `struct itimerspec` consumer patch). `build-macos.sh` injects epoll-shim's
include dir and link so the timers resolve; no change to the upstream project.

## Build

```bash
./build-macos.sh          # on macOS; builds libwayland then the scanner + examples
```

Installs/consumes `~/wlroots-darwin/prefix`. CI (`wayland-cxx-scanner on macOS`)
compiles + links it on Apple Silicon. Running the shm examples needs a
compositor and a GUI session — see `../RUNNING.md`.

## Layout

- `pin.env` — pinned wayland-cxx-scanner + wayland-protocols revisions.
- `build-macos.sh` — build runbook (delegates to wayland-darwin for libwayland).
- `../.github/workflows/wayland-cxx-macos.yml` — the CI workflow.
