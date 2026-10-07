# Y11 - core X11 display server daemon for The Y11 Project.
# POSIX make: no GNU extensions (no wildcard, no patsubst, no :=).

CC       = cc
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L $(DRM_INCS)
LDFLAGS  =

# OS detection via uname -s.  The "!=" shell-assignment operator is
# supported by BSD make and by GNU make >= 4.0.  FreeBSD keeps its ports
# tree under /usr/local, so its include and library paths are appended;
# other systems (e.g. Linux) use the compiler defaults.
UNAME_S != uname -s
FreeBSD_INCS = -I/usr/local/include
FreeBSD_LIBS = -L/usr/local/lib
INCS      = -Iinclude $($(UNAME_S)_INCS)
LIBS      = $($(UNAME_S)_LIBS)

# DRM/KMS and seat: libdrm via pkg-config (headers + lib), libseat via its
# vendored header (include/libseat.h) linked by soname where the dev package
# is absent.  The "!=" shell assignment is plain BSD/GNU make, not $(shell).
DRM_INCS  != pkg-config --cflags libdrm 2>/dev/null || echo
DRM_LIBS  != pkg-config --libs libdrm 2>/dev/null || echo -ldrm
Linux_SEATLIB  = -l:libseat.so.1
FreeBSD_SEATLIB = -lseat
SEATLIB  = $($(UNAME_S)_SEATLIB)

HDRS = include/y11.h include/y11_wire.h include/y11_drm.h include/libseat.h
OBJS = src/main.o src/client.o src/dispatch.o src/atom.o src/resource.o src/events.o src/window.o src/pixmap.o src/gc.o src/render.o src/damage.o src/input.o src/grab.o src/session.o src/drm.o src/scanout.o src/shm.o src/dri3.o src/present.o src/property.o src/glx.o src/selection.o src/xrender.o

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
DESTDIR =

all: y11

y11: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS) $(DRM_LIBS) $(SEATLIB)

src/main.o: src/main.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/main.c -o $@

src/client.o: src/client.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/client.c -o $@

src/dispatch.o: src/dispatch.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/dispatch.c -o $@

src/atom.o: src/atom.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/atom.c -o $@

src/resource.o: src/resource.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/resource.c -o $@

src/events.o: src/events.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/events.c -o $@

src/window.o: src/window.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/window.c -o $@

src/pixmap.o: src/pixmap.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/pixmap.c -o $@

src/gc.o: src/gc.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/gc.c -o $@

src/render.o: src/render.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/render.c -o $@

src/damage.o: src/damage.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/damage.c -o $@

src/input.o: src/input.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/input.c -o $@

src/grab.o: src/grab.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/grab.c -o $@

src/session.o: src/session.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/session.c -o $@

src/drm.o: src/drm.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/drm.c -o $@

src/scanout.o: src/scanout.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/scanout.c -o $@

src/shm.o: src/shm.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/shm.c -o $@

src/dri3.o: src/dri3.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/dri3.c -o $@

src/present.o: src/present.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/present.c -o $@

src/property.o: src/property.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/property.c -o $@

src/glx.o: src/glx.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/glx.c -o $@

src/selection.o: src/selection.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/selection.c -o $@

src/xrender.o: src/xrender.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/xrender.c -o $@

clean:
	rm -f $(OBJS) y11

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 y11 $(DESTDIR)$(BINDIR)/y11

.PHONY: all clean install
