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
| peer creds (`SO_PEERCRED` / `ucred`) | `wayland-os.c` | ✅ `LOCAL_PEERCRED` / `struct xucred` branch already present |
| `MSG_CMSG_CLOEXEC`, `SOCK_CLOEXEC` fallbacks | `wayland-os.c` / `.h` | ✅ portable fallbacks already present |

epoll-shim covers macOS as a tested target (its README lists macOS 13.7.1). It
ships all four headers (`sys/{epoll,timerfd,signalfd,eventfd}.h`) and has
`__APPLE__`-specific paths in the kqueue backend (`epollfd_ctx.c`,
`signalfd_ctx.c`, `compat_sigops.c`, `compat_kqueue1.c`) plus Apple compat
targets (`pipe2`, `socket`, `socketpair`, `itimerspec`, `sem`, `ppoll`).

## The build patch

`patches/0001-wayland-darwin-build-support.patch` is two edits to wayland's
top-level `meson.build`, verified to apply cleanly to the pinned tree:

- Add `'darwin'` to the epoll-shim branch, so macOS pulls the kqueue-backed
  shim instead of assuming native epoll.
- Exclude `'darwin'` from the strict `_POSIX_C_SOURCE=200809L` definition —
  defining it on macOS *hides* the BSD extensions the OS-abstraction layer
  needs (`LOCAL_PEERCRED`, `struct xucred`, `u_int`).

Both are upstreamable on the existing FreeBSD/OpenBSD precedent.

epoll-shim itself needs no patch (macOS is an upstream-tested target).

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

- **Client peer PID unavailable.** macOS `struct xucred` has no `cr_pid`, so
  `wl_client_get_credentials()` returns pid = -1 unless a `LOCAL_PEERPID` path
  is added. Fine for most compositors; note it if you key on client pid.
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
2. libwayland (with the patch) configures — the meson `SFD_CLOEXEC` /
   `TFD_CLOEXEC` / `CLOCK_MONOTONIC` probes resolve through epoll-shim — and
   `meson compile` + `meson install` succeed.
3. `meson test` (libwayland's own suite: connection, event loop, socket /
   `SCM_RIGHTS` fd passing, display roundtrip) passes.
4. `eventloop-smoke` prints `PASS` — fd(epoll) + timer(timerfd) +
   signal(signalfd) + idle all fire in one dispatch cycle.

## Building

- **On a Mac:** `./build-macos.sh` (self-bootstrapping — clones the pinned
  sources into `../src` if absent, installs Homebrew deps, builds, runs the
  acceptance test).
- **Via CI (no Mac needed):** the `libwayland on macOS` GitHub Actions workflow
  runs the same on `macos-13` (Intel) + `macos-14` / `macos-15` (Apple
  Silicon). Trigger it by pushing, or manually via *workflow_dispatch*.

## Layout

- `pin.env` — pinned source URLs + commit SHAs (single source of truth).
- `patches/0001-wayland-darwin-build-support.patch` — the two-edit meson patch.
- `build-macos.sh` — build + acceptance runbook (self-bootstraps sources).
- `eventloop-smoke.c` — targeted event-loop smoke over the epoll-shim surface.
- `../.github/workflows/macos.yml` — the CI workflow.
