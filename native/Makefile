CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: mx-night-light-native

mx-night-light-native: mx-night-light-native.c
	$(CC) $(CFLAGS) -o $@ $<

install: mx-night-light-native
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 mx-night-light-native $(DESTDIR)$(BINDIR)/mx-night-light-native

clean:
	rm -f mx-night-light-native

.PHONY: all install clean
