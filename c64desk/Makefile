CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Wno-format-truncation $(shell pkg-config --cflags xft)
LDLIBS = $(shell pkg-config --libs x11 xft xext xfixes xrandr) -lm -lutil
PREFIX ?= /usr/local
OBJS = main.o items.o ui.o console.o vice.o editor.o wallpaper.o basic.o library.o popup.o tile.o menus.o png.o

c64desk: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): c64desk.h

install: c64desk
	install -Dm755 c64desk $(DESTDIR)$(PREFIX)/bin/c64desk

clean:
	rm -f c64desk $(OBJS)
