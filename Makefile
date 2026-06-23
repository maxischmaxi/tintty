# tintty — Build
# User-Install (kein sudo). Für System-Install: make install PREFIX=/usr/local (mit sudo).
PREFIX  ?= $(HOME)/.local
APPDIR   = $(DESTDIR)$(PREFIX)/share/applications
PKGS     = wayland-client wayland-cursor xkbcommon freetype2 fontconfig pixman-1

WL_PROTO_DIR != pkg-config --variable=pkgdatadir wayland-protocols
WL_SCANNER   != pkg-config --variable=wayland_scanner wayland-scanner

PKG_CFLAGS != pkg-config --cflags $(PKGS)
PKG_LIBS   != pkg-config --libs $(PKGS)

CFLAGS  ?= -Os
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-const-variable \
           -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -I. $(PKG_CFLAGS)
LDFLAGS ?= -s
LIBS     = $(PKG_LIBS) -lutil -lrt -lm

# via wayland-scanner generierter Protokoll-Glue
PROTO_H = xdg-shell-client-protocol.h \
          xdg-decoration-unstable-v1-client-protocol.h \
          fractional-scale-v1-client-protocol.h \
          viewporter-client-protocol.h \
          primary-selection-unstable-v1-client-protocol.h
PROTO_C = xdg-shell-protocol.c \
          xdg-decoration-unstable-v1-protocol.c \
          fractional-scale-v1-protocol.c \
          viewporter-protocol.c \
          primary-selection-unstable-v1-protocol.c
PROTO_O = $(PROTO_C:.c=.o)

OBJ = tintty.o render.o wl.o $(PROTO_O)

all: tintty

config.h:
	cp config.def.h config.h

xdg-shell-client-protocol.h:
	$(WL_SCANNER) client-header $(WL_PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@
xdg-shell-protocol.c:
	$(WL_SCANNER) private-code $(WL_PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@
xdg-decoration-unstable-v1-client-protocol.h:
	$(WL_SCANNER) client-header $(WL_PROTO_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@
xdg-decoration-unstable-v1-protocol.c:
	$(WL_SCANNER) private-code $(WL_PROTO_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@
fractional-scale-v1-client-protocol.h:
	$(WL_SCANNER) client-header $(WL_PROTO_DIR)/staging/fractional-scale/fractional-scale-v1.xml $@
fractional-scale-v1-protocol.c:
	$(WL_SCANNER) private-code $(WL_PROTO_DIR)/staging/fractional-scale/fractional-scale-v1.xml $@
viewporter-client-protocol.h:
	$(WL_SCANNER) client-header $(WL_PROTO_DIR)/stable/viewporter/viewporter.xml $@
viewporter-protocol.c:
	$(WL_SCANNER) private-code $(WL_PROTO_DIR)/stable/viewporter/viewporter.xml $@
primary-selection-unstable-v1-client-protocol.h:
	$(WL_SCANNER) client-header $(WL_PROTO_DIR)/unstable/primary-selection/primary-selection-unstable-v1.xml $@
primary-selection-unstable-v1-protocol.c:
	$(WL_SCANNER) private-code $(WL_PROTO_DIR)/unstable/primary-selection/primary-selection-unstable-v1.xml $@

# alle Objekte brauchen config.h + die generierten Header
$(OBJ): config.h $(PROTO_H)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

tintty: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

clean:
	rm -f tintty $(OBJ) $(PROTO_H) $(PROTO_C) config.h

install: tintty
	install -Dm755 tintty $(DESTDIR)$(PREFIX)/bin/tintty
	install -Dm644 tintty.desktop $(APPDIR)/tintty.desktop
	sed -i 's|^Exec=.*|Exec=$(PREFIX)/bin/tintty|' $(APPDIR)/tintty.desktop
	-update-desktop-database $(APPDIR) 2>/dev/null || true
	@echo "tintty -> $(DESTDIR)$(PREFIX)/bin/tintty"
	@echo "desktop -> $(APPDIR)/tintty.desktop  (in rofi/wofi 'drun' sichtbar)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tintty
	rm -f $(APPDIR)/tintty.desktop
	-update-desktop-database $(APPDIR) 2>/dev/null || true

.PHONY: all clean install uninstall
