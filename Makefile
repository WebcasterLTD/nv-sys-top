PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall -Wextra

NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
NCURSES_LIBS   := $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)

nv-sys-top: src/nv-sys-top.c
	$(CC) $(CFLAGS) $(NCURSES_CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(NCURSES_LIBS) -ldl

install: nv-sys-top
	install -Dm755 nv-sys-top $(DESTDIR)$(PREFIX)/bin/nv-sys-top
	install -Dm644 nv-sys-top.1 $(DESTDIR)$(PREFIX)/share/man/man1/nv-sys-top.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nv-sys-top $(DESTDIR)$(PREFIX)/share/man/man1/nv-sys-top.1

clean:
	rm -f nv-sys-top

.PHONY: install uninstall clean
