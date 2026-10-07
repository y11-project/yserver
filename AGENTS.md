# AGENTS.md

Guidance for AI coding agents working in this repository.

## Project

`y11` (repo: `yserver`) is an X11-compatible display server daemon for
The Y11 Project, written in strict ISO C99 for portable POSIX (Linux
glibc/musl and FreeBSD). It implements the wire protocol engine,
connection handshake, the window tree hierarchy, WM substructure
redirection, core 2D rasterization (drawables, graphics contexts,
solid fills, blits and image transfer), input and grabs, MIT-SHM,
DRI3/Present buffer passing, and a GLX visual bridge for direct
rendering clients.

## Build

- `make` builds `./y11` with `-std=c99 -pedantic -Wall -Wextra -Werror`.
  The build must be warning-free under both gcc and clang.
- `make clean`, `make install` (PREFIX/DESTDIR supported).
- The Makefile is plain POSIX make (no GNU extensions).

## Running and debugging

- `./y11 [display]` binds `/tmp/.X11-unix/X<n>` (default 0) and maintains
  the `/tmp/.y11-unix/Y<n>` symlink. SIGINT/SIGTERM unlink the sockets.
- `Y11_DEBUG=1` logs every request, reply, event and disconnect.
- Without a seat manager (or while another display server holds DRM
  master) y11 logs "no seat available" and runs headless; every
  protocol path still works over the socket. Never take DRM master
  away from another running display server.
- Test against a free display with Xlib or raw-socket clients;
  `xeyes` makes a good real-client smoke test.

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
  `/usr/include/X11/Xproto.h` (and the extension protos) before using
  them; do not trust memory. xcb's generated headers under
  `/usr/include/xcb` are the other authoritative reference.
- Value lists (CreateWindow, ChangeWindowAttributes, ConfigureWindow)
  are 4 bytes per set mask bit, in increasing bit order.
- libseat's API header is vendored at include/libseat.h; the runtime is
  linked by soname (`-l:libseat.so.1`).

## Protocol notes

- ChangeProperty carries its mode in header byte 1 (Xproto.h and xcb
  agree); format sits at body byte 12 with three pad bytes after it.
- CirculateWindow's window is the parent whose children circulate; the
  redirect lock is checked on that window, and `place` sits at event
  byte 16.
- xcb reads GLX IsDirect's `is_direct` from byte 8; the legacy
  xGLXIsDirectReply keeps it at byte 1 — fill both.
- Mesa's DRI3 loader rejects the render fd unless the server reports
  XFIXES 2 or newer.
- DRI3 fences are xshmfence pages: the server mmaps the fd and
  triggers the fence (store + futex wake) when the presented buffer
  becomes idle.
- Mesa matches the advertised GLX visual/fbconfig against its driver
  configs attribute by attribute; the advertised entry must mirror
  what a modern driver reports (no accumulation buffers, bind-to-
  texture on, Y inverted) or every config is dropped as unmatched.
- libXrender requires the connection setup to advertise depths
  1/4/8/24/32 before it will even send QueryExtension("RENDER"); it
  probes missing depths with 1x1 pixmaps and reads the errors.
- Xft without RENDER falls back to core drawing (client-side blend +
  PutImage); with RENDER it uploads A8 glyph masks via AddGlyphs and
  composites with CompositeGlyphs8, whose first element carries the
  absolute pen position in its deltas.
- DRI3Open hands clients the render node fd; the server keeps its own
  card fd for Prime imports and the dumb-buffer CPU maps used by the
  Present blit path.

## Commits

- Conventional Commits, subject only, no body: `feat: ...`, `fix: ...`,
  `docs: ...`.
- Keep commits small; every commit must build on its own.
- Work directly on `master`. Do not create extra branches.
