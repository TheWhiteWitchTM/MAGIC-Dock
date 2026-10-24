CXX ?= g++
CC ?= gcc
SCANNER ?= wayland-scanner
GEN := build/gen
PROTO := protocols

PROTOS := \
	wlr-layer-shell-unstable-v1 \
	ext-foreign-toplevel-list-v1 \
	cosmic-toplevel-info-unstable-v1 \
	cosmic-toplevel-management-unstable-v1 \
	fractional-scale-v1 \
	viewporter \
	xdg-shell \
	ext-workspace-v1 \
	cosmic-workspace-unstable-v1 \
	ext-image-capture-source-v1 \
	ext-image-copy-capture-v1

GEN_HDR := $(addprefix $(GEN)/, $(addsuffix -client.h, $(PROTOS)))
GEN_SRC := $(addprefix $(GEN)/, $(addsuffix -protocol.c, $(PROTOS)))
GEN_OBJ := $(patsubst $(GEN)/%.c, build/%.o, $(GEN_SRC))

# The Qt 6.12 .pc files still name the prefix of the machine that built them.
# Use a tree that actually contains headers and libs: $QT_PREFIX, otherwise the
# first LD_LIBRARY_PATH entry that is a Qt lib directory. Fall back to pkg-config.
QT_PREFIX ?= $(shell \
	found=; \
	if [ -n "$$QT_PREFIX" ] && [ -f "$$QT_PREFIX/include/QtCore/qglobal.h" ] && [ -e "$$QT_PREFIX/lib/libQt6Core.so" ]; then \
		found=$$QT_PREFIX; \
	else \
		IFS=:; \
		for d in $$LD_LIBRARY_PATH; do \
			[ -n "$$d" ] || continue; \
			prefix=$$(dirname "$$d"); \
			if [ -f "$$prefix/include/QtCore/qglobal.h" ] && [ -e "$$d/libQt6Core.so" ]; then \
				found=$$prefix; \
				break; \
			fi; \
		done; \
	fi; \
	printf '%s' "$$found")

ifneq ($(QT_PREFIX),)
QT_CFLAGS := -I$(QT_PREFIX)/include/QtWidgets -I$(QT_PREFIX)/include -DQT_WIDGETS_LIB \
	-I$(QT_PREFIX)/include/QtCore -I$(QT_PREFIX)/mkspecs/linux-g++ \
	-I$(QT_PREFIX)/include/QtGui -DQT_GUI_LIB -DQT_CORE_LIB
QT_LIBS := -L$(QT_PREFIX)/lib -Wl,-rpath-link,$(QT_PREFIX)/lib -lQt6Widgets -lQt6Gui -lQt6Core
# $ORIGIN/../lib is dist/lib when the binary lives in dist/bin. The absolute
# prefix is only a fallback so ./magic-dock runs from the build tree.
QT_RPATH := -Wl,--disable-new-dtags -Wl,-rpath,'$$ORIGIN/../lib' -Wl,-rpath,$(QT_PREFIX)/lib
else
QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6Gui)
QT_LIBS := $(shell pkg-config --libs Qt6Widgets Qt6Gui)
QT_RPATH := -Wl,--disable-new-dtags -Wl,-rpath,'$$ORIGIN/../lib'
endif

CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -DQT_NO_KEYWORDS -fvisibility=hidden -fvisibility-inlines-hidden -mno-direct-extern-access -I$(GEN) -Isrc $(QT_CFLAGS)
CFLAGS := -O2 -Wall -Wno-unused-parameter -I$(GEN)

.PHONY: all clean preview dist test
.SECONDARY: $(GEN_SRC) $(GEN_HDR)

all: magic-dock

$(GEN)/%-client.h: $(PROTO)/%.xml
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
	@sed -i 's/\bnamespace\b/ns_name/g' $@

$(GEN)/%-protocol.c: $(PROTO)/%.xml
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

build/%.o: $(GEN)/%.c $(GEN_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/apps.o: src/apps.cpp src/apps.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c src/apps.cpp -o $@

build/backend.o: src/backend.cpp src/backend.hpp $(GEN_HDR)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c src/backend.cpp -o $@

build/settings.o: src/settings.cpp src/settings.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c src/settings.cpp -o $@

build/dock.o: src/dock.cpp src/dock.hpp src/apps.hpp src/backend.hpp src/settings.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c src/dock.cpp -o $@

build/main.o: src/main.cpp src/dock.hpp src/backend.hpp src/apps.hpp src/settings.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c src/main.cpp -o $@

magic-dock: build/main.o build/dock.o build/backend.o build/apps.o build/settings.o $(GEN_OBJ)
	$(CXX) -rdynamic $(QT_RPATH) -o $@ $^ $(QT_LIBS) -lwayland-client -lwayland-cursor -lpthread

preview: magic-dock
	./magic-dock --preview build/preview

test: magic-dock
	./magic-dock --self-test
	./magic-dock --preview build/preview

dist: magic-dock
	QT_PREFIX="$(QT_PREFIX)" ./scripts/bundle-dist.sh

clean:
	rm -rf build magic-dock cosmic-bar dist
