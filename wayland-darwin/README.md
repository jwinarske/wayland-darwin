# libwayland on macOS (Darwin)

Build support for [libwayland](https://gitlab.freedesktop.org/wayland/wayland)
(`wayland-server`, `wayland-client`, `wayland-cursor`, `wayland-scanner`) on
macOS, with its epoll-based event loop backed by
[epoll-shim](https://github.com/jiixyj/epoll-shim) over `kqueue(2)`. This is
groundwork for bringing Wayland software to macOS.

Everything here is authored against pinned upstream revisions; the actual
compile + runtime acceptance test runs on macOS (locally or via the CI
workflow — no changes to the upstream trees beyond one small build patch).

## Source basis (pinned)

See `pin.env`.

| Component | Repo | Commit | Version |
|---|---|---|---|
| wayland | gitlab.freedesktop.org/wayland/wayland `main` | `6c0a03a8` | 1.26.90 |
| epoll-shim | github.com/jiixyj/epoll-shim `master` | `18159584` | — |

## The Darwin delta is small

libwayland already carries first-class FreeBSD/OpenBSD support, so the
Linux-specific pieces are already abstracted and feature-probed. Full audit of
`src/`:

| Linux primitive | Where | Already portable? |
|---|---|---|
| `epoll_*`, `EPOLL*` | `event-loop.c`, `wayland-os.c` | ✅ via `dependency('epoll-shim')` (meson.build) |
| `timerfd_*`, `signalfd` | `event-loop.c` | ✅ epoll-shim headers; probed in meson |
| `eventfd` | `wayland-server.c` | ✅ epoll-shim provides `<sys/eventfd.h>` on macOS (not native) |
| `memfd_create`, `mkostemp`, `posix_fallocate`, `accept4`, `mremap`, `prctl`, `gettid` | various | ✅ each `HAVE_*`-probed with fallbacks |
| `SOCK_CLOEXEC`, `MSG_CMSG_CLOEXEC` | `wayland-os.c` | ⚠ present natively on the BSDs but **not on macOS** — patched to fall through to the `fcntl(FD_CLOEXEC)` fallback (patch 0002) |
| peer creds (`ucred`) | `wayland-os.c` | ⚠ the BSD/`SO_PEERCRED` branches don't fit Darwin — patched to add a `LOCAL_PEERCRED` + `LOCAL_PEERPID` branch (patch 0002) |
| `MSG_NOSIGNAL` send flag | `connection.c` | ⚠ absent on macOS — patched to `#define` it away and suppress `SIGPIPE` per-socket with `SO_NOSIGPIPE` (patch 0002) |
| `ppoll` | `wayland-client.c` | ⚠ absent on macOS — patched with a `poll(2)`-based fallback (patch 0002) |
| `struct itimerspec` | `event-loop.c` (via epoll-shim `<sys/timerfd.h>`) | ⚠ macOS `<time.h>` doesn't define it and epoll-shim only forward-declares it in its installed header — patched epoll-shim to define it for consumers (epoll-shim patch 0001) |

epoll-shim covers macOS as a tested target (its README lists macOS 13.7.1). It
ships all four headers (`sys/{epoll,timerfd,signalfd,eventfd}.h`) and has
`__APPLE__`-specific paths in the kqueue backend (`epollfd_ctx.c`,
`signalfd_ctx.c`, `compat_sigops.c`, `compat_kqueue1.c`) plus Apple compat
targets (`pipe2`, `socket`, `socketpair`, `itimerspec`, `sem`, `ppoll`).

## The patches

Small patches to the pinned trees, verified to apply cleanly and all
upstreamable — two to wayland (`patches/wayland/`) and one to epoll-shim
(`patches/epoll-shim/`):

**`patches/wayland/0001-wayland-darwin-build-support.patch`** — two edits to the
top-level `meson.build`:

- Add `'darwin'` to the epoll-shim branch, so macOS pulls the kqueue-backed
  shim instead of assuming native epoll.
- Exclude `'darwin'` from the strict `_POSIX_C_SOURCE=200809L` definition —
  defining it on macOS *hides* the BSD extensions the OS-abstraction layer
  needs (`LOCAL_PEERCRED`, `struct xucred`, `u_int`).

**`patches/wayland/0002-os-darwin-cloexec-and-peercred.patch`** — Darwin runtime
portability across `wayland-os.{c,h}` and `wayland-client.c`:

- Guard the `SOCK_CLOEXEC` / `MSG_CMSG_CLOEXEC` fast paths with `#ifdef`, so
  Darwin (which has neither, unlike the BSDs) falls through to the portable
  `fcntl(FD_CLOEXEC)` fallbacks already used for `EPOLL_CLOEXEC` / `accept4`.
- Add a Darwin peer-credentials branch using `LOCAL_PEERCRED` (`struct
  xucred`) for uid/gid and `LOCAL_PEERPID` for the pid.
- `MSG_NOSIGNAL` doesn't exist on macOS: define it away and suppress `SIGPIPE`
  per-socket via `SO_NOSIGPIPE` (`set_nosigpipe`) in the socket/accept paths.
- `ppoll(2)` doesn't exist on macOS: add a `poll(2)`-based fallback in
  `wl_display_poll` (its only caller always passes `sigmask == NULL`).

**`patches/epoll-shim/0001-timerfd-define-itimerspec-on-darwin.patch`** —
epoll-shim's installed `<sys/timerfd.h>` only forward-declares `struct
itimerspec` and relies on `<time.h>` to complete it, which macOS does not.
Define the struct in the installed header on Apple (guarded out during
epoll-shim's own build, where its internal definition is already in scope).

## Notes

- **epoll-shim macro mode.** epoll-shim offers two consumption modes: default
  *macro* mode (`dependency('epoll-shim')`, which redefines
  `read`/`write`/`close` as macros so shim-fd lifecycle is handled correctly)
  and *interpose* mode (`epoll-shim-interpose`, `dlsym`/`RTLD_NEXT` wrappers +
  `-DEPOLL_SHIM_DISABLE_WRAPPER_MACROS`). We use macro mode, matching the
  established FreeBSD port: libwayland localizes each shim-fd type to a single
  translation unit (the event loop; eventfd in `wayland-server.c`), which is
  the pattern the epoll-shim README calls safe. Interpose is the documented
  fallback if a downstream consumer is found closing a shim fd in a TU that
  lacks the shim headers.
- **Private prefix.** epoll-shim and libwayland install into a self-contained
  prefix (`$HOME/wayland-darwin/prefix` by default); nothing touches the
  system. Downstream consumers pick it up via `PKG_CONFIG_PATH`.

## Known limitations on macOS

- **Client peer gid is the primary group only.** macOS `struct xucred` carries
  a group list rather than a single gid, so the peercred branch reports
  `cr_groups[0]`. uid and pid (via `LOCAL_PEERPID`) are exact.
- **`epoll-shim.pc` hardcodes `Libs.private: -pthread -lrt`.** There is no
  `-lrt` on macOS. It is only consulted for `--static`, so it is harmless for
  the default shared build; patch the `.pc` (drop `-lrt` on Apple) if you ever
  need a static link.
- **epoll-shim `EPOLLET` / non-`kqueue`able fds.** Edge-triggering and
  `/dev`-style descriptors have caveats on the shim (see its README).
  libwayland uses level-triggered fd sources, so this is not expected to bite.

## Acceptance test

Considered passing on macOS when all of:

1. epoll-shim builds + installs; `pkg-config --exists epoll-shim` succeeds.
2. libwayland (with the patches) configures — the meson `SFD_CLOEXEC` /
   `TFD_CLOEXEC` / `CLOCK_MONOTONIC` probes resolve through epoll-shim — and
   `meson compile` + `meson install` build the client, server, cursor, and
   scanner libraries.
3. `eventloop-smoke` prints `PASS` — fd(epoll) + timer(timerfd) +
   signal(signalfd) + idle all fire in one `wl_event_loop` dispatch cycle.
4. `roundtrip-smoke` prints `PASS` — a forked client connects to a server over
   a real unix socket and completes a `wl_display_roundtrip`, exercising the
   accept/cloexec path and the wire protocol end to end.

**Not covered yet:** libwayland's own `meson test` suite. Its harness registers
tests via an ELF-only `section()` + `__start/__stop` iteration that Mach-O does
not support, and the tests assume Linux (`/proc/self/fd`, signalfd/timerfd
semantics). Porting the harness (Mach-O `section$start$…` bounds) and auditing
the individual tests is separate follow-up work; the smokes above cover the
core library surface in the meantime.

## Building

- **On a Mac:** `./build-macos.sh` (self-bootstrapping — clones the pinned
  sources into `../src` if absent, installs Homebrew deps, builds, runs the
  acceptance test).
- **Via CI (no Mac needed):** the `libwayland on macOS` GitHub Actions workflow
  runs the same on `macos-13` (Intel) + `macos-14` / `macos-15` (Apple
  Silicon). Trigger it by pushing, or manually via *workflow_dispatch*.

## Layout

- `pin.env` — pinned source URLs + commit SHAs (single source of truth).
- `patches/wayland/0001-wayland-darwin-build-support.patch` — the two-edit meson patch.
- `patches/wayland/0002-os-darwin-cloexec-and-peercred.patch` — `wayland-os.{c,h}` + `wayland-client.c` runtime port.
- `patches/epoll-shim/0001-timerfd-define-itimerspec-on-darwin.patch` — `struct itimerspec` for consumers.
- `build-macos.sh` — build + acceptance runbook (self-bootstraps sources).
- `eventloop-smoke.c` — targeted event-loop smoke over the epoll-shim surface.
- `roundtrip-smoke.c` — client↔server roundtrip over a real unix socket.
- `../.github/workflows/macos.yml` — the CI workflow.
