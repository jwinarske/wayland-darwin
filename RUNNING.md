# Running on a Mac (Apple Silicon)

The build is CI-green on `macos-14`/`macos-15` (arm64), so an M1/M2/M3 Mac builds
the same. The one wrinkle: **windowed apps need a GUI login session**, because
AppKit (`NSWindow`, `CVDisplayLink`) must reach the WindowServer — a plain SSH
shell has no Aqua session. The headless parts (build + the Metal GPU test) run
fine over SSH.

## 1. Prerequisites

```bash
xcode-select --install          # clang + the Metal toolchain
# Homebrew, if not present: https://brew.sh
```

Everything else (`cmake ninja meson pkgconf libffi expat libxml2 libxkbcommon
pixman`) is installed by the build script.

## 2. Get the code

```bash
git clone git@github.com:jwinarske/wayland-darwin.git
cd wayland-darwin
```

The upstream sources (`src/`) are git-ignored and re-fetched at pinned revisions
by the build.

## 3. Build everything + verify the GPU path — works over SSH

```bash
./libwlr-darwin/build-macos.sh
```

Builds libwayland → wlroots (+ the `libdrm-compat` shim) → `libwlr-darwin`,
installs into `~/wlroots-darwin/prefix`, and runs the headless acceptance smokes
— including `metal-smoke`, which renders on the GPU into an IOSurface and reads
it back. Expect to see:

```
 PASS — libwayland builds and runs on macOS via epoll-shim
 PASS — wlroots core builds AND runs (headless+pixman) on macOS
metal smoke: PASS (rect + textured client surface)
```

Built binaries land in `libwlr-darwin/build-macos/`.

## 4. See a window — needs a GUI session (not plain SSH)

Get an Aqua session remotely via **Screen Sharing** (System Settings → General →
Sharing → Screen Sharing), then use **Terminal.app inside that session**. Or be
at the machine.

```bash
cd ~/wayland-darwin
export DYLD_LIBRARY_PATH="$HOME/wlroots-darwin/prefix/lib"
B=libwlr-darwin/build-macos

# A) a plain window: blue fill via pixman -> zero-copy IOSurface -> CALayer
"$B/darwin-smoke"

# B) the compositor (Metal renderer; pixman fallback). Prints WAYLAND_DISPLAY.
"$B/darwin-tinywl"
```

`darwin-tinywl` prints e.g. `WAYLAND_DISPLAY=wayland-1` and opens an empty
compositor window. Keyboard/mouse in either window route into wlroots.

## 5. Show a real client

From a second Terminal in the same GUI session:

```bash
cd ~/wayland-darwin
export DYLD_LIBRARY_PATH="$HOME/wlroots-darwin/prefix/lib"
WAYLAND_DISPLAY=wayland-1 libwlr-darwin/build-macos/wl-client-demo
```

`wl-client-demo` is a minimal libwayland-client program (xdg-shell + shm) that
draws a checkerboard. It should appear as a tiled surface inside the
`darwin-tinywl` window, composited on the GPU by the Metal renderer.

(Existing Linux Wayland apps like `foot` are not macOS binaries; running them
would need the `waypipe` bridge into a Linux container — not built yet.
`wl-client-demo` is the native macOS client for now.)

## 6. C++ example clients (wayland-cxx-scanner)

Build the C++ Wayland bindings + example clients (the GL/Vulkan/SDL3/Skia ones
self-skip on macOS):

```bash
./wayland-cxx-scanner/build-macos.sh
```

The CPU/shm examples that build on macOS (the GL/Vulkan/SDL3/Skia ones don't).
Run each in the GUI session, with `darwin-tinywl` already running (step 4):

```bash
export DYLD_LIBRARY_PATH="$HOME/wlroots-darwin/prefix/lib"
export WAYLAND_DISPLAY=wayland-1                 # matches what darwin-tinywl printed
E=src/wayland-cxx-scanner/build-macos/examples

# — no window (connect / introspect) —
"$E/minimal/minimal_roundtrip"          # connect + wl_display roundtrip, then exit
"$E/wayland-info/wayland_info"          # print the compositor's globals
"$E/ext-data-control/ext-data-control"  # focus-free clipboard CLI (copy/paste)

# — shm windows (composited on the GPU by darwin-tinywl's Metal renderer) —
"$E/presentation-shm/presentation_shm" # animated spinning wheel + frame timings
"$E/key-input/key_input"               # keyboard input + key-repeat echo
"$E/clipboard/clipboard"               # wl_data_device selection (clipboard)
"$E/text-input/text_input"             # a text-field window
"$E/xdg-ssd/xdg_ssd"                   # server-side-decoration window
"$E/agl-presentation-shm/agl_compositor"  # AGL shell background client
"$E/ivi-presentation-shm/ivi_shell"       # IVI-application client
```

These are pure `libwayland-client` C++ programs — no wlroots — talking to the
Darwin `wl_display` socket, their shm buffers composited on the GPU by the Metal
renderer in `darwin-tinywl`.

## Notes

- In-window keyboard/mouse need no special permissions (only *global* event
  taps would prompt for Input Monitoring).
- If a windowed app exits immediately with a WindowServer/`NSApplication`
  error, you are in a non-GUI session — use Screen Sharing (step 4).
- `metal-smoke` also runs standalone over SSH to confirm the GPU path:
  `DYLD_LIBRARY_PATH=$HOME/wlroots-darwin/prefix/lib libwlr-darwin/build-macos/metal-smoke`.
