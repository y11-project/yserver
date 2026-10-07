# AGENTS.md

Guidance for AI coding agents working in this repository.

## Project

`y11` (repo: `yserver`) is an X11-compatible display server daemon for
The Y11 Project, written in strict ISO C99 for portable POSIX (Linux
glibc/musl and FreeBSD). It implements the wire protocol engine,
connection handshake, the window tree hierarchy, WM substructure
redirection, and core 2D rasterization (drawables, graphics contexts,
solid fills, blits and image transfer).

## Build

- `make` builds `./y11` with `-std=c99 -pedantic -Wall -Wextra -Werror`.
  The build must be warning-free under both gcc and clang.
- `make clean`, `make install` (PREFIX/DESTDIR supported).
- The Makefile is plain POSIX make (no GNU extensions).

## Running and debugging

- `./y11 [display]` binds `/tmp/.X11-unix/X<n>` (default 0) and maintains
  the `/tmp/.y11-unix/Y<n>` symlink. SIGINT/SIGTERM unlink the sockets.
- `Y11_DEBUG=1` logs every request, reply, event and disconnect.

## Code style

- ISO C99 only, no GNU extensions. POSIX via `-D_POSIX_C_SOURCE=200809L`.
- Internal identifiers use the `y11_` and `yid_` prefixes.
- Wire structs live in `include/y11_wire.h`, are naturally aligned (no
  packing attributes) and match the X11 protocol byte-for-byte. Every
  struct gets a compile-time size check (`y11_wire_chk_*`).
- Multi-byte wire fields are only accessed through the little-endian
  helpers `y11_wire_get16/32` and `y11_wire_put16/32`; single bytes may
  be accessed directly.
- Look opcodes, reply layouts and event layouts up in
  `/usr/include/X11/Xproto.h` before using them; do not trust memory.
- Value lists (CreateWindow, ChangeWindowAttributes, ConfigureWindow)
  are 4 bytes per set mask bit, in increasing bit order.

## Commits

- Conventional Commits, subject only, no body: `feat: ...`, `fix: ...`,
  `docs: ...`.
- Keep commits small; every commit must build on its own.
- Work directly on `master`. Do not create extra branches.

## Verification on this host

- A foreign X server holds the abstract `:0` socket, which libX11/XCB
  prefers, so run the daemon on a free display (`./y11 5`) and test
  with `DISPLAY=:5`.
- This host has real DRM hardware (`/dev/dri/card1`, i915, eDP panel,
  card2 is an `appletbdrm` touchbar; there is no amdgpu here) but the
  user's sway session owns DRM master, so y11 cannot take scanout
  here: it logs "no seat available" and runs headless. All hardware
  paths (seat, KMS discovery, dumb buffers, master gate) are exercised
  by the `ycard` harness in `/tmp/opencode`; dumb-buffer ioctls work
  unprivileged on the card node, modesetting correctly fails with
  EACCES while another display server holds master. Never steal DRM
  master from the running session.
- libseat has no dev package installed: the API header is vendored at
  include/libseat.h and the runtime is linked by soname
  (`-l:libseat.so.1`).
- The i915 render node (`renderD128`) refuses `CREATE_DUMB` with
  EPERM, so y11 keeps its own card fd for Prime imports and dumb
  CPU maps, while DRI3Open hands clients the render node exactly
  like a real server (Mesa allocates real GEM buffers there and
  never needs dumb buffers). DRI3/Present are verified end-to-end
  with the `ydri3` harness (fd receipt, DMA-BUF round trip, present
  events, pixel verification).
- GLX is the visual bridge in src/glx.c: Mesa matches the advertised
  visual/fbconfig against its driver configs attribute by attribute
  (accum 0, bind-to-texture on, Y inverted, alpha 8 for the RGBA8888
  config) and builds real contexts client-side through the DRI3
  render node. glxinfo reports GLX 1.4 with direct rendering on the
  Iris Plus GPU and glxgears renders. Hard-won wire facts: xcb reads
  GLX IsDirect's `is_direct` from byte 8 (the legacy struct keeps it
  at byte 1, fill both); Mesa's DRI3Open rejects the fd unless the
  server reports XFIXES 2 or newer; DRI3 fences are xshmfence pages
  the server triggers on Present completion.
- ChangeProperty carries its mode in header byte 1 (Xproto.h and
  xcb agree); CirculateWindow's window is the parent whose children
  circulate, and the redirect lock is checked on that window.
- dwm builds from dl.suckless.org here and runs on y11: it acquires
  SubstructureRedirectMask (a second dwm correctly gets BadAccess),
  manages clients via MapRequest and routes WM_DELETE_WINDOW. Its bar
  text needs Xft/RENDER which is absent, so drw skips fonts and the
  bar renders without letters.
- Client harnesses are built from source into `/tmp/opencode` (the
  approved temp area gets wiped occasionally; rebuild as needed):
  `xwininfo` (needs its libxcb-shape stub), `xsetroot`, `yimg`
  (image round-trip suite), `ycirc` (circulation), `yclose`
  (WM_DELETE routing), `yglx` (GLX probe) and the `ioctllog`/
  `conflog`/`isdirect` LD_PRELOAD shims used to debug Mesa. `xeyes`
  is installed system-wide and is the real-client smoke test.
- `xmessage` cannot be built here (no Xaw headers); use `xeyes` plus the
  raw-socket harnesses instead.
