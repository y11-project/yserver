# Y11 display server.
# Copyright (c) 2026 The Y11 Project
# SPDX-License-Identifier: BSD-2-Clause

CC       = cc
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L $(DRM_INCS)
LDFLAGS  =

UNAME_S  != uname -s
FreeBSD_INCS = -I/usr/local/include
FreeBSD_LIBS = -L/usr/local/lib
INCS     = -Iinclude $($(UNAME_S)_INCS)
LIBS     = $($(UNAME_S)_LIBS)

DRM_INCS != pkg-config --cflags libdrm 2>/dev/null || echo
DRM_LIBS != pkg-config --libs libdrm 2>/dev/null || echo -ldrm
Linux_SEATLIB    = -l:libseat.so.1
FreeBSD_SEATLIB  = -lseat
SEATLIB  = $($(UNAME_S)_SEATLIB)

HDRS     = include/y11.h include/wire.h include/kms.h include/libseat.h include/safe.h
OBJDIR   = out/artifacts
OBJS     = $(OBJDIR)/main.o $(OBJDIR)/client.o $(OBJDIR)/dispatch.o $(OBJDIR)/atom.o $(OBJDIR)/resource.o $(OBJDIR)/events.o $(OBJDIR)/window.o $(OBJDIR)/pixmap.o $(OBJDIR)/gc.o $(OBJDIR)/render.o $(OBJDIR)/damage.o $(OBJDIR)/input.o $(OBJDIR)/grab.o $(OBJDIR)/session.o $(OBJDIR)/drm.o $(OBJDIR)/scanout.o $(OBJDIR)/shm.o $(OBJDIR)/dri3.o $(OBJDIR)/present.o $(OBJDIR)/property.o $(OBJDIR)/glx.o $(OBJDIR)/selection.o $(OBJDIR)/xrender.o $(OBJDIR)/evdev.o $(OBJDIR)/xkb.o $(OBJDIR)/cursor.o $(OBJDIR)/saver.o

# The wire-protocol validators are a mandatory part of the server.
RUSTC    = rustc
SAFESRC  = rust/safe/src/lib.rs rust/safe/src/error.rs rust/safe/src/error_frame.rs rust/safe/src/frame.rs rust/safe/src/handshake.rs rust/safe/src/mask.rs rust/safe/src/opcode.rs rust/safe/src/property.rs rust/safe/src/list.rs rust/safe/src/shm.rs rust/safe/src/reply.rs
SAFELIB  = $(OBJDIR)/libsafe.a

PREFIX   = /usr/local
BINDIR   = $(PREFIX)/bin
DESTDIR  =

all: out/y11

$(OBJS): $(OBJDIR)/.dir

$(SAFELIB): $(SAFESRC)
	$(RUSTC) --edition 2021 --crate-name safe --crate-type staticlib -C panic=abort -O -o $@ rust/safe/src/lib.rs

out/y11: $(OBJS) $(SAFELIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(SAFELIB) $(LIBS) $(DRM_LIBS) $(SEATLIB)

$(OBJDIR)/.dir:
	@mkdir -p $(OBJDIR)
	@touch $@

$(OBJDIR)/main.o: src/main.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/main.c -o $@

$(OBJDIR)/client.o: src/client.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/client.c -o $@

$(OBJDIR)/dispatch.o: src/dispatch.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/dispatch.c -o $@

$(OBJDIR)/atom.o: src/atom.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/atom.c -o $@

$(OBJDIR)/resource.o: src/resource.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/resource.c -o $@

$(OBJDIR)/events.o: src/events.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/events.c -o $@

$(OBJDIR)/window.o: src/window.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/window.c -o $@

$(OBJDIR)/pixmap.o: src/pixmap.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/pixmap.c -o $@

$(OBJDIR)/gc.o: src/gc.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/gc.c -o $@

$(OBJDIR)/render.o: src/render.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/render.c -o $@

$(OBJDIR)/damage.o: src/damage.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/damage.c -o $@

$(OBJDIR)/input.o: src/input.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/input.c -o $@

$(OBJDIR)/grab.o: src/grab.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/grab.c -o $@

$(OBJDIR)/session.o: src/session.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/session.c -o $@

$(OBJDIR)/drm.o: src/drm.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/drm.c -o $@

$(OBJDIR)/scanout.o: src/scanout.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/scanout.c -o $@

$(OBJDIR)/shm.o: src/shm.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/shm.c -o $@

$(OBJDIR)/dri3.o: src/dri3.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/dri3.c -o $@

$(OBJDIR)/present.o: src/present.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/present.c -o $@

$(OBJDIR)/property.o: src/property.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/property.c -o $@

$(OBJDIR)/glx.o: src/glx.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/glx.c -o $@

$(OBJDIR)/selection.o: src/selection.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/selection.c -o $@

$(OBJDIR)/xrender.o: src/xrender.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/xrender.c -o $@

$(OBJDIR)/evdev.o: src/evdev.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/evdev.c -o $@

$(OBJDIR)/xkb.o: src/xkb.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/xkb.c -o $@

$(OBJDIR)/cursor.o: src/cursor.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/cursor.c -o $@

$(OBJDIR)/saver.o: src/saver.c $(HDRS)
	$(CC) $(CFLAGS) $(INCS) -c src/saver.c -o $@

clean:
	rm -rf out

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 out/y11 $(DESTDIR)$(BINDIR)/y11

.PHONY: all clean install
