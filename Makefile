CC      ?= cc
PKGS    := wayland-client cairo pango pangocairo xkbcommon

CFLAGS  ?= -march=native -O2 -g -flto -s
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

PROTO_SRC = wlr-layer-shell-protocol.c cursor-shape-protocol.c \
            xdg-shell-protocol.c tablet-protocol.c xdg-decoration-protocol.c

TARGETS := jtwpi jtwpictl

all: $(TARGETS)

jtwpi: jtwpi.c jticons.h xdg-shell-client-protocol.h xdg-decoration-client-protocol.h $(PROTO_SRC)
	$(CC) $(CFLAGS) -o $@ jtwpi.c $(PROTO_SRC) $(LDFLAGS) $(LDLIBS)

jtwpictl: jtwpictl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

# Regenerate the layer-shell bindings if the XML is updated.
wlr-layer-shell-protocol.c wlr-layer-shell-client-protocol.h: wlr-layer-shell-unstable-v1.xml
	wayland-scanner private-code $< wlr-layer-shell-protocol.c
	wayland-scanner client-header $< wlr-layer-shell-client-protocol.h

install: $(TARGETS)
	install -Dm755 jtwpi $(DESTDIR)$(BINDIR)/jtwpi
	install -Dm755 jtwpictl $(DESTDIR)$(BINDIR)/jtwpictl

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jtwpi
	rm -f $(DESTDIR)$(BINDIR)/jtwpictl

clean:
	rm -f $(TARGETS)

.PHONY: all install uninstall clean
