# C64 style LightDM greeter. needs liblightdm-gobject-1, x11, xft
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Wno-format-truncation $(shell pkg-config --cflags liblightdm-gobject-1 gobject-2.0 glib-2.0 x11 xft)
LDLIBS = $(shell pkg-config --libs liblightdm-gobject-1 gobject-2.0 glib-2.0 x11 xft)
PREFIX ?= /usr/local

c64pine-greeter: c64pine-greeter.c

install: c64pine-greeter
	install -Dm755 c64pine-greeter $(DESTDIR)$(PREFIX)/sbin/c64pine-greeter
	install -Dm644 c64pine-greeter.desktop $(DESTDIR)/usr/share/xgreeters/c64pine-greeter.desktop

clean:
	rm -f c64pine-greeter
