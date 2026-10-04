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
- Client harnesses are built from source into `/tmp/opencode` (the
  approved temp area gets wiped occasionally; rebuild as needed):
  `xwininfo` (needs its libxcb-shape stub), `xsetroot`, and `yimg`
  (image round-trip suite). `xeyes` is installed system-wide and is
  the real-client smoke test.
- `xmessage` cannot be built here (no Xaw headers); use `xeyes` plus the
  raw-socket harnesses instead.
