# Pluto: a small, modular wlroots compositor (fork of dwl).
#
# The compositor is split into one translation unit per subsystem under src/.
# Generated Wayland protocol headers live in build/.

include config.mk

BUILD = build
BIN   = pluto

SRC = \
	src/main.c \
	src/output.c \
	src/client.c \
	src/seat.c \
	src/layer.c \
	src/layout.c \
	src/layout_math.c \
	src/protocols.c \
	src/config.c \
	src/ime.c \
	src/xwayland.c \
	src/listeners.c

OBJ = $(SRC:src/%.c=$(BUILD)/%.o)

# wayland-scanner generates the protocol headers that wlroots' own headers
# expect to find in the include path.  Upstream dwl generates some of these;
# wlroots 0.20 also needs enum headers for the wayland-protocols it references.
# Keep this list in sync with the wlroots headers included by include/pluto.h.
WAYLAND_SCANNER   := $(or $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2>/dev/null),wayland-scanner)
WAYLAND_PROTOCOLS := $(or $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null),/usr/share/wayland-protocols)

GEN = \
	$(BUILD)/xdg-shell-protocol.h \
	$(BUILD)/wlr-layer-shell-unstable-v1-protocol.h \
	$(BUILD)/wlr-output-power-management-unstable-v1-protocol.h \
	$(BUILD)/wayland-protocols/cursor-shape-v1-enum.h \
	$(BUILD)/wayland-protocols/ext-image-copy-capture-v1-enum.h \
	$(BUILD)/wayland-protocols/pointer-constraints-unstable-v1-enum.h \
	$(BUILD)/wayland-protocols/xdg-shell-enum.h

# flags for compiling
PLUTOCPPFLAGS = -Iinclude -I. -I$(BUILD) -DWLR_USE_UNSTABLE \
	-D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\" $(XWAYLAND) $(CPPFLAGS)
# Pluto itself only uses C99 features, but wlroots' headers use anonymous
# unions (C11) and the config invariants use _Static_assert (C11).  Compile as
# gnu11 explicitly instead of relying on the compiler default.
PLUTODEVCFLAGS = -std=gnu11 -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# pixman/libdrm are public dependencies of wlroots headers but some wlroots
# pkg-config files omit them from Cflags.
PKGS       = wayland-server xkbcommon libinput pixman-1 libdrm $(XLIBS)
PLUTOCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(PLUTOCPPFLAGS) \
	$(PLUTODEVCFLAGS) $(CFLAGS)
LDLIBS     = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm $(LIBS)

all: $(BIN)

$(BIN): $(OBJ) config.h
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/%.o: src/%.c config.h $(GEN)
	@mkdir -p $(dir $@)
	$(CC) -c -MMD -MP $(PLUTOCFLAGS) $< -o $@

-include $(OBJ:.o=.d)

$(BUILD)/xdg-shell-protocol.h:
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

$(BUILD)/wlr-layer-shell-unstable-v1-protocol.h:
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@

$(BUILD)/wlr-output-power-management-unstable-v1-protocol.h:
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@

$(BUILD)/wayland-protocols/cursor-shape-v1-enum.h:
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@

$(BUILD)/wayland-protocols/ext-image-copy-capture-v1-enum.h:
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@

$(BUILD)/wayland-protocols/pointer-constraints-unstable-v1-enum.h:
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@

$(BUILD)/wayland-protocols/xdg-shell-enum.h:
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

config.h:
	cp config.def.h $@

TEST = $(BUILD)/layout_math_test

$(TEST): tests/layout_math_test.c src/layout_math.c include/layout_math.h
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -g -Wall -Wextra -Werror -Iinclude \
		tests/layout_math_test.c src/layout_math.c -lm -o $@

test: $(TEST)
	./$(TEST)

clean:
	rm -rf $(BUILD) $(BIN)

dist: clean
	mkdir -p $(BIN)-$(VERSION)
	cp -R LICENSE* Makefile CHANGELOG.md README.md ARCHITECTURE.md \
		PORTING.md include src tests config.def.h config.mk protocols \
		pluto.1 pluto.desktop start.sh flake.nix flake.lock $(BIN)-$(VERSION)
	tar -caf $(BIN)-$(VERSION).tar.gz $(BIN)-$(VERSION)
	rm -rf $(BIN)-$(VERSION)

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f pluto.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/pluto.1
	mkdir -p $(DESTDIR)$(DATADIR)/wayland-sessions
	cp -f pluto.desktop $(DESTDIR)$(DATADIR)/wayland-sessions
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/pluto.desktop
	mkdir -p $(DESTDIR)$(DATADIR)/pluto
	cp -f start.sh $(DESTDIR)$(DATADIR)/pluto/start.sh
	chmod 755 $(DESTDIR)$(DATADIR)/pluto/start.sh

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(MANDIR)/man1/pluto.1 \
		$(DESTDIR)$(DATADIR)/wayland-sessions/pluto.desktop \
		$(DESTDIR)$(DATADIR)/pluto/start.sh

.PHONY: all clean dist install test uninstall
