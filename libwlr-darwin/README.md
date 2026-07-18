# libwlr-darwin — a Cocoa backend for wlroots

A nested, windowed wlroots backend for macOS: each `wlr_output` is presented as
an NSWindow. Built **out of tree** against wlroots' public unstable interfaces
(no wlroots fork), on top of the `wayland-darwin` libwayland and the
`wlroots-darwin` core build.

Status: **scaffold** — the backend/output/thread-bridge skeleton is in place and
the C side compiles against the real wlroots interfaces; the Objective-C/AppKit
side (`cocoa.m`) is authored and compile/link-checked on macOS CI. Presenting a
real window needs a login session, so the demo is not run in CI.

## Threading model (normative)

AppKit owns the process **main thread**; `wl_display` and every wlroots call
live on a dedicated **compositor thread**. No wlroots symbol is ever touched
from the main thread, and no AppKit call is made off it.

```
main() ──► wlr_darwin_application_run(compositor_main, data)
             │  (on the main thread)
             ├─ NSApplication sharedApplication + [NSApp run]   ← main thread
             └─ spawns a thread ─► compositor_main(data)        ← compositor thread
                                     wl_display_create()
                                     wlr_darwin_backend_create(loop)
                                     wlr_backend_start()  ─┐
                                     wl_display_run()       │
                                                            ▼
   compositor ──(dispatch_async/sync to main queue)──► NSWindow / CALayer ops
   compositor ◄──(socketpair fds)── CVDisplayLink ticks (frames) + input events
```

- **compositor → main** (window create, present, destroy): marshalled via
  `dispatch_{sync,async}(dispatch_get_main_queue(), …)` in `cocoa.m`.
- **main → compositor** (frame clock, input): written to per-purpose fds that
  the compositor's `wl_event_loop` reads (`wl_event_loop_add_fd`). The frame
  clock is a `CVDisplayLink` tick today (CADisplayLink upgrade noted); input
  serialization is the next workstream.

## Files

| File | Language | Role |
|---|---|---|
| `include/wlr-darwin.h` | C | public API: `wlr_darwin_application_run`, `wlr_darwin_backend_create`, `wlr_darwin_add_output` |
| `src/darwin.h` | C | internal backend/output structs |
| `src/backend.c` | C | `wlr_backend_impl` + one virtual keyboard/pointer + input-record decode → wlr events |
| `src/output.c` | C | `wlr_output_impl` (test/commit/destroy) + present + frame clock |
| `src/allocator.c` | C | IOSurface allocator: `wlr_allocator_interface` + `wlr_buffer_impl` |
| `src/keymap.c` | C | kVK → evdev key-code table (the single maintained pivot) |
| `src/input.h` | C | main→compositor input wire format |
| `src/cocoa.h` | C | the C↔ObjC boundary |
| `src/cocoa.m` | ObjC (ARC) | NSApp trampoline, NSWindow/CALayer, IOSurface, present, CVDisplayLink, NSEvent capture |
| `example/darwin-smoke.c` | C | minimal compositor: opens a window, renders a colour each frame |

All Objective-C is confined to `cocoa.m` behind the `cocoa.h` C boundary.

## Presenting

Two paths, chosen per buffer at commit:

- **Zero-copy (`wlr_darwin_allocator_create`)** — the IOSurface allocator hands
  the pixman renderer IOSurface-backed buffers; the renderer draws straight into
  IOSurface memory (`IOSurfaceLock` ↔ `begin/end_data_ptr_access`), and commit
  assigns the same IOSurface directly to the window's `CALayer.contents`. No copy.
- **Copy fallback** — foreign buffers (e.g. a plain shm allocator, so
  `wlr_allocator_autocreate` keeps working) are wrapped in a `CGImage` and
  copied to the layer.

A Metal renderer is the accelerated path after this (IOSurface is the shared
currency: `MTLTexture` from the same IOSurface).

## Build

`./build-macos.sh` (on macOS) builds the whole stack (libwayland → wlroots →
libwlr-darwin) into a private prefix. CI (`libwlr-darwin (Cocoa backend) on
macOS`) compiles + links it on Apple Silicon.

To see a window, run the installed `darwin-smoke` from a normal login session
(not over headless SSH/CI).

## Input

The backend owns one virtual keyboard + one pointer (D5c). On the main thread,
the content view captures NSEvents and serializes them (`input.h` records) to
the main→compositor fd; the backend decodes them into `wlr_keyboard` /
`wlr_pointer` events:

- **Keyboard** — `NSEvent.keyCode` (kVK) → evdev via `keymap.c`; `wlr_keyboard_notify_key`
  with `update_state` so the compositor-owned xkb keymap tracks modifiers.
  `flagsChanged` drives modifier press/release; key repeat is left client-side.
- **Pointer** — absolute motion (window coords normalized to the output),
  buttons (`BTN_LEFT/RIGHT/MIDDLE`), and scroll: precise (trackpad) → FINGER
  axis source, otherwise WHEEL with discrete steps.
- NSTouch/gestures are not yet surfaced (indirect touch must **not** become
  `wlr_touch`); pointer-gesture mapping is a later addition.

## Next

- A Metal renderer (accelerated path) sampling the same IOSurfaces.
- Window resize → `wlr_output_send_request_state`; multi-output; `backingScaleFactor`.
- tinywl on the Darwin backend.
