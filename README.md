# y11

`y11` is an unprivileged X11 display server written from scratch in ISO C99 for Linux and FreeBSD. It runs directly on top of modern DRM/KMS atomic modesetting and DRI3/DMA-BUF hardware acceleration without sharing any legacy code with Xorg. Following the classic Unix principle of separating mechanism from policy, the server manages screen scanout, buffer passing, and event routing, while window placement is left entirely to external window managers.

## Building

Requires a C99 compiler and standard POSIX make.

```sh
make
make install PREFIX=/usr/local

```

## Running

Start on the default display (`:0`):

```sh
./y11

```

Or pass a specific display index:

```sh
./y11 5

```

Set `Y11_DEBUG=1` to print wire protocol packets to stderr:

```sh
Y11_DEBUG=1 ./y11

```

## Testing

Verify the server handshake with standard X11 tools:

```sh
DISPLAY=:0 xprop -root
DISPLAY=:0 xset q

```

## License

BSD 2-Clause License. See `LICENSE` for details.
