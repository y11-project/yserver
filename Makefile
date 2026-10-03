# Y11 - core X11 display server daemon for The Y11 Project.
# POSIX make: no GNU extensions (no wildcard, no patsubst, no :=).

CC       = cc
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L
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

HDRS = include/y11.h include/y11_wire.h
OBJS = src/main.o src/client.o src/dispatch.o src/atom.o src/resource.o src/events.o src/window.o src/pixmap.o

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
DESTDIR =

all: y11

y11: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

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

clean:
	rm -f $(OBJS) y11

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 y11 $(DESTDIR)$(BINDIR)/y11

.PHONY: all clean install
