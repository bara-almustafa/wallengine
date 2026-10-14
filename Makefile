# wallengine - live wallpaper engine for X11 with Wallpaper Engine (Steam workshop) support
CC      ?= cc
CXX     ?= c++
PREFIX  ?= $(HOME)/.local
DATADIR  = $(PREFIX)/share/wallengine
CFLAGS  ?= -O2 -g
CXXFLAGS ?= -O2

DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1)
DBUS_LIBS   := $(shell pkg-config --libs dbus-1)
# optional: web wallpapers through WebKitGTK
WEBKIT_PKG  := $(shell pkg-config --exists webkit2gtk-4.1 && echo webkit2gtk-4.1 || (pkg-config --exists webkit2gtk-4.0 && echo webkit2gtk-4.0))
ifneq ($(WEBKIT_PKG),)
WEBKIT_CFLAGS := -DHAVE_WEBKIT $(shell pkg-config --cflags $(WEBKIT_PKG))
WEBKIT_LIBS   := $(shell pkg-config --libs $(WEBKIT_PKG))
endif

CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-missing-field-initializers \
           -D_GNU_SOURCE -pthread $(DBUS_CFLAGS)
LDLIBS   = -lX11 -lXext -lGL -llz4 -lm -ldl -pthread $(DBUS_LIBS) $(WEBKIT_LIBS) -lstdc++

SRC = src/main.c src/x11.c src/render.c src/video.c src/audio.c src/stb_impl.c \
      src/we/core.c src/we/project.c src/we/texture.c src/we/shader.c src/we/pass.c src/we/image.c \
      src/we/particle.c src/we/text.c src/we/sound.c src/we/script.c src/we/media.c src/we/scene.c \
      src/we/we.c src/we/web.c src/third_party/cJSON.c
QJS = $(addprefix src/third_party/quickjs/, cutils.c libregexp.c libunicode.c quickjs.c xsum.c)
OBJ = $(SRC:%.c=build/%.o) $(QJS:%.c=build/%.o)

# patched glslang + SPIRV-Cross (C++, used through their C APIs)
GLS = src/third_party/glslang
SPC = src/third_party/spirv-cross
GLS_SRC = $(wildcard $(GLS)/glslang/MachineIndependent/*.cpp $(GLS)/glslang/MachineIndependent/preprocessor/*.cpp \
          $(GLS)/glslang/GenericCodeGen/*.cpp $(GLS)/glslang/CInterface/*.cpp $(GLS)/glslang/ResourceLimits/*.cpp \
          $(GLS)/glslang/OSDependent/Unix/*.cpp $(GLS)/SPIRV/*.cpp $(GLS)/SPIRV/CInterface/*.cpp)
SPC_SRC = $(addprefix $(SPC)/, spirv_cross.cpp spirv_parser.cpp spirv_cross_parsed_ir.cpp spirv_cfg.cpp \
          spirv_glsl.cpp spirv_cross_c.cpp spirv_cross_util.cpp)
SHC_OBJ = $(patsubst %.cpp,build/%.o,$(GLS_SRC) $(SPC_SRC))
SHC_FLAGS = -std=c++17 -fPIC -w -include climits -include cstdint -DENABLE_SPIRV -DENABLE_OPT=0 \
            -DGLSLANG_OSINCLUDE_UNIX -I$(GLS) -I$(GLS)/gen -DSPIRV_CROSS_C_API_GLSL=1

# Run from the source tree: samples are found next to the sources.
SAMPLES ?= $(CURDIR)/samples

all: wallengine

wallengine: $(OBJ) build/libshaderc.a
	$(CC) $(CFLAGS) -o $@ $(OBJ) build/libshaderc.a $(LDLIBS)

build/libshaderc.a: $(SHC_OBJ)
	ar rcs $@ $^

build/src/main.o: src/main.c src/we.h src/we/wallpaper_engine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DSAMPLES_DIR='"$(SAMPLES)"' -c -o $@ $<

build/src/stb_impl.o: src/stb_impl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c -o $@ $<

build/src/third_party/%.o: src/third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -DNDEBUG -c -o $@ $<

build/src/we/web.o: src/we/web.c src/we/internal.h src/we.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WEBKIT_CFLAGS) -c -o $@ $<

build/src/we/%.o: src/we/%.c src/we/internal.h src/we/wallpaper_engine.h src/we/wemath.h src/we.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/src/%.o: src/%.c src/we.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SHC_FLAGS) -c -o $@ $<

install:
	rm -f build/src/main.o
	$(MAKE) SAMPLES=$(DATADIR)/samples
	install -Dm755 wallengine $(PREFIX)/bin/wallengine
	mkdir -p $(DATADIR)
	cp -r samples $(DATADIR)/
	install -Dm644 docs/icon.svg $(PREFIX)/share/icons/hicolor/scalable/apps/wallengine.svg
	install -Dm644 docs/icon.png $(PREFIX)/share/icons/hicolor/512x512/apps/wallengine.png
	install -Dm644 contrib/wallengine.desktop $(HOME)/.config/autostart/wallengine.desktop
	test -f $(HOME)/.config/wallengine/config.ini || \
		install -Dm644 contrib/config.ini $(HOME)/.config/wallengine/config.ini
	@echo "installed to $(PREFIX)/bin/wallengine (autostart entry added)"

uninstall:
	rm -f $(PREFIX)/bin/wallengine $(HOME)/.config/autostart/wallengine.desktop
	rm -f $(PREFIX)/share/icons/hicolor/scalable/apps/wallengine.svg $(PREFIX)/share/icons/hicolor/512x512/apps/wallengine.png
	rm -rf $(DATADIR)

clean:
	rm -f wallengine $(filter-out $(SHC_OBJ),$(OBJ))

distclean:
	rm -rf wallengine build

.PHONY: all install uninstall clean distclean
