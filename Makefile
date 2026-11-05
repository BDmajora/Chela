# Chela: a C core, a toolkit-agnostic C interface, and pluggable UI backends.
#
#   build/chela                    core + UI + native Wayland and headless backends (no GTK, Qt or GLib)
#   build/backends/chela-qt.so     optional Qt 6 host   (make qt)
#   build/backends/chela-gtk.so    optional GTK 3 host  (make gtk)
CC ?= cc
CXX ?= c++
BUILD ?= build
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow -Wno-override-init -pthread
CXXFLAGS += -std=c++17 -Wall -Wextra -pthread -fPIC
CPPFLAGS += -Isrc -I$(BUILD) -I$(BUILD)/protocols
LDLIBS += -ldl -lm -pthread
ARCH ?= x86_64-linux-gnu
DEPS := .deps/usr
D := $(DEPS)/include

# Dependencies come from pkg-config, or from headers unpacked by scripts/local-deps.sh.
PKGS = json-c libcurl sqlite3 wayland-client wayland-cursor xkbcommon cairo fontconfig freetype2
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(shell pkg-config --exists $(PKGS) wayland-protocols wayland-scanner 2>/dev/null && echo yes),)
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))
PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
else ifneq ($(wildcard $(D)/wayland-client.h),)
CPPFLAGS += -I$(D) -I$(D)/$(ARCH) $(addprefix -I$(D)/,cairo freetype2 pixman-1 libpng16)
LDLIBS += $(addprefix -l:lib,json-c.so.5 curl.so.4 sqlite3.so.0 wayland-client.so.0 wayland-cursor.so.0 \
	xkbcommon.so.0 cairo.so.2 fontconfig.so.1 freetype.so.6)
PROTOCOLS := $(DEPS)/share/wayland-protocols
SCANNER := $(DEPS)/bin/wayland-scanner
else
$(error Install dependencies (see README.md) or run: bash scripts/local-deps.sh)
endif
endif

CORE = $(addprefix src/core/,base.c proc.c keyfile.c uri.c core.c store.c sse.c net.c tools.c agent.c acp.c \
	inferssd.c secret.c markdown.c load.c image.c attach.c sandbox.c policy.c)
UI = $(addprefix src/ui/,cu.c text.c edit.c icons.c)
APP = $(addprefix src/app/,app.c prefs.c widgets.c menus.c dialogs.c sidebar.c chat.c composer.c model.c \
	monitor.c tunnel.c voice.c askpass.c attach_ui.c panel.c)
PLATFORM = $(addprefix src/platform/,cairo_canvas.c headless.c wayland.c)
CORE_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(CORE))
OBJ = $(CORE_OBJ) $(patsubst src/%.c,$(BUILD)/%.o,$(UI) $(APP) $(PLATFORM) src/main.c)

XML = stable/xdg-shell/xdg-shell.xml unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
	staging/fractional-scale/fractional-scale-v1.xml stable/viewporter/viewporter.xml \
	staging/cursor-shape/cursor-shape-v1.xml stable/tablet/tablet-v2.xml \
	unstable/text-input/text-input-unstable-v3.xml
PROTO_NAMES = $(basename $(notdir $(XML)))
PROTO_H = $(patsubst %,$(BUILD)/protocols/%-client-protocol.h,$(PROTO_NAMES))
PROTO_C = $(patsubst %,$(BUILD)/protocols/%.c,$(PROTO_NAMES))
PROTO_OBJ = $(PROTO_C:.c=.o)

.PHONY: all qt gtk backends clean test integration keyring-test ui-test desktop-test library-ui-test \
	connections-ui-test monitor-ui-test model-status-ui-test attachments-ui-test workspace-ui-test library-test download-test wayland-test check sanitize install
all: $(BUILD)/chela
backends: qt gtk

$(BUILD)/protocols/%-client-protocol.h:
	@mkdir -p $(dir $@)
	$(SCANNER) client-header $(PROTOCOLS)/$(filter %/$*.xml,$(XML)) $@
$(BUILD)/protocols/%.c:
	@mkdir -p $(dir $@)
	$(SCANNER) private-code $(PROTOCOLS)/$(filter %/$*.xml,$(XML)) $@
$(BUILD)/protocols/%.o: $(BUILD)/protocols/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/inferssd_library.h: scripts/inferssd-library.py scripts/embed-resource.py
	@mkdir -p $(BUILD)
	python3 scripts/embed-resource.py $< $@
$(BUILD)/core/inferssd.o: $(BUILD)/inferssd_library.h
$(BUILD)/platform/wayland.o: $(PROTO_H)
$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/chela: $(OBJ) $(PROTO_OBJ)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

# Optional toolkit hosts, loaded at run time with --backend=qt or --backend=gtk.
QT_MODULES = Qt6Widgets Qt6Gui Qt6Core
ifneq ($(shell pkg-config --exists $(QT_MODULES) 2>/dev/null && echo yes),)
QT_CFLAGS = $(shell pkg-config --cflags $(QT_MODULES))
QT_LIBS = $(shell pkg-config --libs $(QT_MODULES))
else
QT_INC = $(D)/$(ARCH)/qt6
QT_CFLAGS = -I$(QT_INC) $(addprefix -I$(QT_INC)/,QtWidgets QtGui QtCore) -DQT_NO_VERSION_TAGGING
QT_LIBS = $(addprefix -l:lib,Qt6Widgets.so.6 Qt6Gui.so.6 Qt6Core.so.6)
endif
qt: $(BUILD)/backends/chela-qt.so
$(BUILD)/backends/chela-qt.so: src/platform/qt.cpp src/ui/platform.h
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(QT_CFLAGS) $(CXXFLAGS) -shared $< $(QT_LIBS) -o $@

GTK_PKGS = gtk+-3.0
ifneq ($(shell pkg-config --exists $(GTK_PKGS) 2>/dev/null && echo yes),)
GTK_CFLAGS = $(shell pkg-config --cflags $(GTK_PKGS))
GTK_LIBS = $(shell pkg-config --libs $(GTK_PKGS))
else
GTK_CFLAGS = $(addprefix -I$(D)/,gtk-3.0 glib-2.0 pango-1.0 harfbuzz cairo atk-1.0 gdk-pixbuf-2.0) \
	-I$(DEPS)/lib/$(ARCH)/glib-2.0/include
GTK_LIBS = $(addprefix -l:lib,gtk-3.so.0 gdk-3.so.0 gobject-2.0.so.0 glib-2.0.so.0 cairo.so.2)
endif
gtk: $(BUILD)/backends/chela-gtk.so
$(BUILD)/backends/chela-gtk.so: src/platform/gtk.c src/platform/cairo_canvas.c src/core/base.c src/ui/platform.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(GTK_CFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -shared src/platform/gtk.c \
		src/platform/cairo_canvas.c src/core/base.c $(GTK_LIBS) $(LDLIBS) -o $@

# Tests
$(BUILD)/test_core: tests/test_core.c $(CORE_OBJ)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) $^ $(LDLIBS) -o $@
test: $(BUILD)/test_core
	$(BUILD)/test_core
integration: $(BUILD)/test_core
	CHELA_TEST_BINARY='$(abspath $(BUILD))/test_core' python3 tests/integration.py
$(BUILD)/test-keyring/libsecret-1.so.0: tests/mock_secret.c src/core/base.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC $^ -l:libglib-2.0.so.0 -pthread -o $@
keyring-test: $(BUILD)/test_core $(BUILD)/test-keyring/libsecret-1.so.0
	F=$$(mktemp -d) && CHELA_FIXTURE=$$F CHELA_TEST_KEYRING=1 LD_LIBRARY_PATH='$(abspath $(BUILD))/test-keyring' \
		$(BUILD)/test_core -p /credentials; S=$$?; rm -rf "$$F"; exit $$S
# Desktop tests drive build/chela with the headless backend: no display or input tools.
desktop-test: $(BUILD)/chela
	CHELA_BINARY='$(abspath $(BUILD))/chela' python3 tests/ui_headless.py
library-ui-test: $(BUILD)/chela $(BUILD)/test_core $(BUILD)/test-keyring/libsecret-1.so.0
	python3 tests/model_library_ui.py
connections-ui-test: $(BUILD)/chela
	python3 tests/connections_ui.py
monitor-ui-test: $(BUILD)/chela
	python3 tests/monitor_ui.py
model-status-ui-test: $(BUILD)/chela
	python3 tests/model_status_ui.py
attachments-ui-test: $(BUILD)/chela
	python3 tests/attachments_ui.py
workspace-ui-test: $(BUILD)/chela
	python3 tests/workspace_ui.py
ui-test: desktop-test library-ui-test connections-ui-test monitor-ui-test model-status-ui-test attachments-ui-test workspace-ui-test
# Paste through the real Wayland backend, inside a private nested KWin.
WL_TEST = $(BUILD)/wayland-test
$(WL_TEST)/paste-client: tests/wayland_paste_client.c tests/fake-input.xml
	@mkdir -p $(WL_TEST)
	$(SCANNER) client-header $(PROTOCOLS)/staging/ext-data-control/ext-data-control-v1.xml $(WL_TEST)/ext-data-control-v1-client-protocol.h
	$(SCANNER) private-code $(PROTOCOLS)/staging/ext-data-control/ext-data-control-v1.xml $(WL_TEST)/ext-data-control.c
	$(SCANNER) client-header tests/fake-input.xml $(WL_TEST)/fake-input-client-protocol.h
	$(SCANNER) private-code tests/fake-input.xml $(WL_TEST)/fake-input.c
	$(CC) $(CPPFLAGS) -I$(WL_TEST) -O1 -g tests/wayland_paste_client.c $(WL_TEST)/ext-data-control.c $(WL_TEST)/fake-input.c $(LDLIBS) -o $@
$(WL_TEST)/drag-client: tests/wayland_drag_client.c tests/fake-input.xml $(WL_TEST)/paste-client $(PROTO_H)
	$(CC) $(CPPFLAGS) -I$(WL_TEST) -O1 -g tests/wayland_drag_client.c $(WL_TEST)/fake-input.c $(BUILD)/protocols/xdg-shell.o $(LDLIBS) -o $@
$(WL_TEST)/input-client: tests/wayland_input_client.c tests/fake-input.xml $(WL_TEST)/paste-client
	$(CC) $(CPPFLAGS) -I$(WL_TEST) -O1 -g tests/wayland_input_client.c $(WL_TEST)/fake-input.c $(LDLIBS) -o $@
wayland-test: $(BUILD)/chela $(WL_TEST)/paste-client $(WL_TEST)/drag-client $(WL_TEST)/input-client
	python3 tests/wayland_paste.py
library-test:
	python3 tests/test_model_library.py
download-test:
	python3 tests/test_download_models.py
check: test integration keyring-test ui-test library-test download-test wayland-test
sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' test integration
install: $(BUILD)/chela
	install -Dm755 $(BUILD)/chela $(DESTDIR)$(HOME)/.local/bin/chela
	for b in $(wildcard $(BUILD)/backends/*.so); do install -Dm755 $$b $(DESTDIR)$(HOME)/.local/lib/chela/$$(basename $$b); done
	install -Dm644 packaging/chela.desktop $(DESTDIR)$(HOME)/.local/share/applications/chela.desktop
	install -Dm644 assets/chela.svg $(DESTDIR)$(HOME)/.local/share/icons/hicolor/scalable/apps/chela.svg
clean:
	rm -rf build
-include $(wildcard $(BUILD)/*/*.d $(BUILD)/*.d)
