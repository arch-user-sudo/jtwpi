CC      ?= cc
PKGS    := gtk+-3.0 gtk-layer-shell-0 gio-2.0 gdk-pixbuf-2.0

CFLAGS  ?= -march=native -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

TARGET  := jtwpi
SRC     := jtwpi.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
