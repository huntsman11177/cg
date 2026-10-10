PREFIX ?= /usr/local
DESTDIR ?=
CC ?= cc
CFLAGS ?= -O3 -Wall -Wextra
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -pthread

VERSION := $(shell cat VERSION 2>/dev/null || echo 1.2.2)

.PHONY: all clean check install install-man uninstall

all: cg

cg: cg.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

check: cg
	./tests/smoke.sh ./cg

install: cg install-man
	install -Dm755 cg "$(DESTDIR)$(PREFIX)/bin/cg"

install-man:
	install -Dm644 man/cg.1 "$(DESTDIR)$(PREFIX)/share/man/man1/cg.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/cg"
	rm -f "$(DESTDIR)$(PREFIX)/share/man/man1/cg.1"

clean:
	rm -f cg
