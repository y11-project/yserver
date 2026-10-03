# Y11 — the core display server daemon for The Y11 Project

`yserver` (binary: `y11`) is the core X11 display server daemon for
**The Y11 Project**, written in strict ISO C99 for portable POSIX systems
(Linux glibc/musl and FreeBSD).

## Phase 1 — Headless Protocol Engine & Handshake

Phase 1 implements the protocol engine without any rendering or input:

- **Connection setup handshake** — full successful setup reply (release
  110000, per-client resource id space, vendor "The Y11 Project", pixmap
  formats, screen/depth/visual information).
- **Non-blocking poll(2) event loop** — UNIX domain socket at
  `/tmp/.X11-unix/X0` (alias `/tmp/.y11-unix/Y0`), level-triggered
  `poll(2)`, no busy waiting.
- **Strict sequence tracking** — every request increments the client's
  sequence number; every reply/error carries it in bytes 2-3.
- **BIG-REQUESTS extension** — advertised via QueryExtension, enabled by
  its 4-byte Enable request; oversized requests use the 8-byte extended
  header with the true 4-byte-unit length.
- **Atom subsystem** — predefined X11 core atoms 1-68 in a static array,
  dynamic atoms (69+) in a hash table; `InternAtom` (16) and
  `GetAtomName` (17).
- **Core queries** — `ListProperties` (21, empty list), `GetProperty`
  (20, property-not-present reply), `GetInputFocus` (43), `GetFontPath`
  (52), `GetKeyboardControl` (103), `GetPointerControl` (106),
  `GetScreenSaver` (108), `NoOperation` (127).

Requests that y11 does not implement are answered with proper X11 error
packets rather than hanging the client.  Phase 1 speaks the little-endian
wire encoding only; clients that request big-endian byte order (`'B'`)
receive a failed setup reply and are disconnected.  Resource id ranges
are non-overlapping per client: client N owns `[(N+1) * 0x00100000,
... + 0x000FFFFF]`.

## Layout

```
yserver/
├── Makefile
├── README.md
├── include/
│   ├── y11.h          # Internal definitions, client context, server state, yid_t
│   └── y11_wire.h     # 1-byte packed structs matching X11 wire protocol specs
└── src/
    ├── main.c         # Socket creation, signal traps, non-blocking poll() loop
    ├── client.c       # Client allocation, ring buffer management, handshake parsing
    ├── dispatch.c     # Opcode dispatcher, BIG-REQUESTS handling, sequence tracking
    └── atom.c         # Atoms 1-68 preload array, dynamic string hash table
```

## Building

```sh
make            # builds ./y11 with -std=c99 -pedantic -Wall -Wextra -Werror
make clean
make install    # installs the binary to $(PREFIX)/bin (default /usr/local)
```

The Makefile is plain POSIX make (no GNU extensions) and detects FreeBSD
vs Linux via `uname -s` to append the proper include/library paths.

## Running

```sh
./y11            # bind /tmp/.X11-unix/X0 (display 0)
./y11 5          # bind /tmp/.X11-unix/X5 instead
```

The daemon creates the socket (unlinking any stale socket first), the
`/tmp/.y11-unix/Y<n>` alias symlink, and listens without spinning.
SIGINT/SIGTERM unlink the sockets and free all allocations.  Set
`Y11_DEBUG=1` to log a line per incoming request and reply.

## Verifying

```sh
DISPLAY=:0 xprop -root    # connects, queries root properties, exits 0
DISPLAY=:0 xset q         # connects, queries vendor/version state, exits 0
```

## License

See the repository settings; code is provided for The Y11 Project.
