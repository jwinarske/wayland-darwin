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
  clock is a `CVDisplayLink` tick today (CADisplayLink upgrade noted); each tick
  carries the vsync timestamp + refresh + counter that drive both the `frame`
  and `present` events.

## Files

| File | Language | Role |
|---|---|---|
| `include/wlr-darwin.h` | C | public API: `wlr_darwin_application_run`, `wlr_darwin_backend_create`, `wlr_darwin_add_output` |
| `src/darwin.h` | C | internal backend/output structs |
| `src/backend.c` | C | `wlr_backend_impl` + the virtual keyboard + output count (`WLR_DARWIN_OUTPUTS`) |
| `src/output.c` | C | `wlr_output_impl` (test/commit/destroy) + present + frame clock + hardware cursor + per-output pointer + input decode |
| `src/allocator.c` | C | IOSurface allocator: `wlr_allocator_interface` + `wlr_buffer_impl` |
| `src/keymap.c` | C | kVK → evdev key-code table (the single maintained pivot) |
| `src/input.h` | C | main→compositor input wire format |
| `src/renderer.c` | C | Metal renderer: `wlr_renderer_impl` + `wlr_render_pass_impl` + format negotiation |
| `src/data.c` | C | NSPasteboard ↔ seat-selection clipboard bridge |
| `src/pasteboard.m` | ObjC | NSPasteboard get/set/change-count (main thread) |
| `src/metal.m` | ObjC (ARC) | Metal device/pipeline, IOSurface render target, solid-rect pass, readback |
| `src/cocoa.h` / `src/metal.h` | C | the C↔ObjC boundaries |
| `src/cocoa.m` | ObjC (ARC) | NSApp trampoline, NSWindow/CALayer, IOSurface, present, CVDisplayLink, NSEvent capture |
| `example/darwin-smoke.c` | C | minimal compositor: opens a window, renders a colour each frame |
| `example/tinywl.c` | C | wlroots' reference compositor, adapted to the Darwin backend (Metal renderer) |
| `example/wl-client-demo.c` | C | minimal Wayland client (xdg-shell + shm) to display in the compositor |

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

To see a window, run `darwin-smoke` (a colour fill) or `darwin-tinywl` from a
normal login session (not over headless SSH/CI). `darwin-tinywl` is wlroots'
reference compositor with ~10 lines changed (the application trampoline plus
`wlr_darwin_backend_create` / `wlr_darwin_allocator_create`); a Wayland client
launched into its `WAYLAND_DISPLAY` renders as tiled surfaces in the window.

## Input

The backend owns one virtual keyboard; each output owns its own `wlr_pointer`
(its `output_name` binds absolute motion to that window, so the cursor lands in
the right output). On the main thread, each window's content view captures
NSEvents and serializes them (`input.h` records) to that output's
main→compositor fd; `output.c` decodes them — key events to the shared keyboard,
pointer events to that output's pointer:

- **Keyboard** — `NSEvent.keyCode` (kVK) → evdev via `keymap.c`; `wlr_keyboard_notify_key`
  with `update_state` so the compositor-owned xkb keymap tracks modifiers.
  `flagsChanged` drives modifier press/release; key repeat is left client-side.
- **Pointer** — absolute motion (window coords normalized to the output),
  buttons (`BTN_LEFT/RIGHT/MIDDLE`), and scroll: precise (trackpad) → FINGER
  axis source, otherwise WHEEL with discrete steps.
- **Gestures** — `magnifyWithEvent`/`rotateWithEvent` map to a Wayland pinch
  (`pointer-gestures-v1`): scale from magnification, rotation from rotate
  (sign-flipped to clockwise). `darwin-tinywl` creates the gestures global and
  forwards `wlr_cursor` pinch events to clients.
- NSTouch on trackpads stays indirect touch (feeds scroll/gestures); it must
  **not** become `wlr_touch`. macOS has no continuous 3-finger swipe event that
  maps cleanly to Wayland swipe, so only pinch is surfaced for now.

## Metal renderer (accelerated path)

`wlr_darwin_metal_renderer_create()` renders directly into the allocator's
IOSurfaces via Metal (`newTextureWithDescriptor:iosurface:`) — no copy, same
IOSurface currency as the software path. LINEAR-only (DRM XRGB8888/ARGB8888 ↔
`MTLPixelFormatBGRA8Unorm`). Metal render-to-texture is headless-capable, so this
is the one Darwin piece CI can **run**: `metal-smoke` renders a red rect into an
IOSurface and reads it back.

It handles both solid-colour rects (backgrounds) and **client-surface
texturing**: `texture_from_buffer` uploads a client's pixels to an `MTLTexture`,
and `add_texture` samples it onto a quad (src/dst boxes, alpha, nearest/bilinear)
via a textured pipeline. Blend modes are distinct pipelines (premultiplied
alpha-over vs. `NONE` replace), `update_from_buffer` re-uploads only the damaged
sub-regions (`replaceRegion:` per rect), and `render_timer` reports GPU
execution time from the command buffer (`GPUEndTime − GPUStartTime`).
`metal-smoke` verifies all of it headless — rect, texture, transform,
`read_pixels`, both blend modes, and a damage-region upload — read back from the
IOSurface.

## Clipboard

`wlr_darwin_data_bridge_create(seat)` bridges the seat selection to the macOS
system clipboard (text): copying in a Wayland client writes NSPasteboard, and a
macOS copy is published as the Wayland selection (polled via the pasteboard
change count, with loop-avoidance). `darwin-tinywl` creates it after the seat.

## Cursor

The output implements a **hardware cursor** (`set_cursor` / `move_cursor`): the
cursor image lives on its own `CALayer` above the content layer, so wlroots
keeps it off the primary buffer and moving it only repositions that layer — the
scene is never recomposited. Our own IOSurface cursor buffers are assigned to
the layer zero-copy; foreign buffers take a `CGImage` copy. Because the content
view is flipped, the overlay uses the same top-left, upright IOSurface
convention as the primary buffer (no manual Y flip). Reposition/upload run in an
action-disabled `CATransaction`, so the cursor tracks the pointer without
implicit animation.

## Multi-output

Each `wlr_output` is a separate NSWindow with its own frame clock, resize
stream, present path, hardware cursor, and pointer — so more than one is just
more windows. Set `WLR_DARWIN_OUTPUTS=N` to start N of them (default 1;
mirroring the X11 backend's `WLR_X11_OUTPUTS`); a compositor can also call
`wlr_darwin_add_output()` directly to add or hot-plug outputs at runtime. In
`darwin-tinywl` they tile side by side via `wlr_output_layout_add_auto`.

## Present timing

Presentation feedback is driven off the `CVDisplayLink`. A buffer commit is
handed to WindowServer immediately but only turns to light at the next vsync, so
`output.c` defers the `wlr_output` present event: it records the pending commit
(and whether it was zero-copy), and on the following display-link tick sends
`wlr_output_send_present` with the vsync's timestamp (`CVTimeStamp.hostTime`, in
the `CLOCK_MONOTONIC` domain), refresh interval, and monotonic vsync counter —
flagged `VSYNC | HW_CLOCK` (plus `ZERO_COPY` for IOSurface buffers). This gives
`wp_presentation` clients real, hardware-derived timing instead of a synthetic
stamp. If the display link reports no host time, it falls back to
`clock_gettime` and drops the `HW_CLOCK` flag.

## Next

- CADisplayLink (macOS 14+) to replace the deprecated CVDisplayLink.

Metal `add_texture` now bakes `wl_output_transform` into the sampled UVs,
`read_pixels` reads a texture back (screencopy), and IOSurface-backed client
buffers are wrapped zero-copy instead of uploaded — all checked by `metal-smoke`.

HiDPI (backingScaleFactor → output scale, backing-pixel mode) and live window
resize (NSWindow resize / display change → `wlr_output_send_request_state`) are
handled: `cocoa.m` posts the backing size+scale on `setFrameSize` /
`viewDidChangeBackingProperties`, and `output.c` turns each into a state request.
