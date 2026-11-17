CC      ?= cc
PKGS    := gtk+-3.0 gtk-layer-shell-0 gio-2.0 gdk-pixbuf-2.0

CFLAGS  ?= -march=native -O2 -g -flto -s
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

TARGETS := jtwpi jtwpictl

all: $(TARGETS)

jtwpi: jtwpi.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

jtwpictl: jtwpictl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGETS)
	install -Dm755 jtwpi $(DESTDIR)$(BINDIR)/jtwpi
	install -Dm755 jtwpictl $(DESTDIR)$(BINDIR)/jtwpictl

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jtwpi
	rm -f $(DESTDIR)$(BINDIR)/jtwpictl

clean:
	rm -f $(TARGETS)

.PHONY: all install uninstall clean
