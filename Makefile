# Sonix Recovery.
#
#   make                 MODEL=r1 (default)  -> build/r1/sonix_recovery
#   make MODEL=r3proii                       -> build/r3proii/sonix_recovery
#   make host [MODEL=..]                     -> build/host-<model>/sonix_recovery_sim
#                                               (the PC simulator, see src/sim_platform.c)
#   make shots                               host build, then screenshots of every
#                                               screen into build/shots-<model>/
#   make clean
#
# The cross toolchain and LVGL are the player's: ../sonix-player/sonix-player
# has rockbox-toolchain/ and lvgl/ (the player's own Makefile clones LVGL on
# its first run, so run `make target` there once first; the folder has to be
# called lvgl, sources include "lvgl/lvgl.h"). LVGL is built once
# into build/lvgl/ and shared by both models.

MODEL ?= r1

PLAYER_DIR ?= $(CURDIR)/../sonix-player/sonix-player
LVGL_DIR ?= $(PLAYER_DIR)/lvgl
CROSS ?= $(PLAYER_DIR)/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-
# FreeType, for the MiSans face (src/style.c): the player's cross build of it
# (made by its `make target`), and for the simulator a host build of the same
# source, made here out of tree (the player's folder is not touched).
FT_TARGET = $(PLAYER_DIR)/freetype-target
FT_SRC = $(PLAYER_DIR)/freetype-2.13.3
CC = $(CROSS)gcc
STRIP = $(CROSS)strip
READELF = $(CROSS)readelf

ifeq ($(MODEL),r1)
MODEL_DEF = -DMODEL_R1
else ifeq ($(MODEL),r3proii)
MODEL_DEF = -DMODEL_R3PROII
else
$(error MODEL must be r1 or r3proii)
endif

VERSION := $(shell git -C $(CURDIR) describe --always --dirty 2>/dev/null || echo unknown)

# -march=mips32r2 -mno-madd4, glibc_compat.h: the player's reasons, see its
# Makefile. The device has glibc 2.22, the toolchain 2.27; check-abi below
# catches a symbol the device would not have.
CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -MMD -MP -D_FILE_OFFSET_BITS=64 \
		 -march=mips32r2 -mno-madd4 -ffunction-sections -fdata-sections \
		 -include $(PLAYER_DIR)/src/system/core/glibc_compat.h \
		 -I$(CURDIR) -I$(LVGL_DIR)/.. -I$(LVGL_DIR) -I$(FT_TARGET)/include/freetype2 -DLV_CONF_INCLUDE_SIMPLE=1
LVGL_CFLAGS = $(filter-out -Wextra -Wno-unused-parameter,$(CFLAGS)) -w
APP_CFLAGS = $(CFLAGS) $(MODEL_DEF) -DSONIX_RECOVERY_VERSION='"$(VERSION)"'
LDFLAGS = -static-libgcc -Wl,--gc-sections
LDLIBS = $(FT_TARGET)/lib/libfreetype.a -lpthread -lm

BUILD = build/$(MODEL)
BIN = $(BUILD)/sonix_recovery

ALL_SRCS := $(wildcard src/*.c)
APP_SRCS := $(ALL_SRCS)
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -type f -name '*.c' 2>/dev/null)
LVGL_OBJS = $(LVGL_SRCS:$(LVGL_DIR)/src/%.c=build/lvgl/%.o)

# The device build leaves out the simulator; the simulator leaves out what
# talks to the hardware.
SIM_SRCS := $(wildcard src/sim_*.c)
HW_SRCS := src/device.c src/storage.c src/input.c
APP_SRCS := $(filter-out $(SIM_SRCS),$(APP_SRCS))
APP_OBJS = $(APP_SRCS:src/%.c=$(BUILD)/%.o)

.PHONY: all clean check-abi host shots
all: $(BIN)

$(LVGL_DIR)/lvgl.h:
	@echo "no LVGL in $(LVGL_DIR): run 'make target' in $(PLAYER_DIR) first" >&2; exit 1

$(FT_TARGET)/lib/libfreetype.a:
	@echo "no $@: run 'make target' in $(PLAYER_DIR) first (it builds FreeType)" >&2; exit 1

$(APP_OBJS) $(LVGL_OBJS): | $(FT_TARGET)/lib/libfreetype.a

$(BIN): $(APP_OBJS) $(LVGL_OBJS)
	@echo "link $@"
	@$(CC) $(LDFLAGS) -o $@.debug $^ $(LDLIBS)
	$(STRIP) -o $@ $@.debug
	@$(MAKE) --no-print-directory check-abi
	@echo "built $@ ($(MODEL), $(VERSION), $$(du -h $@ | cut -f1)); symbols in $@.debug"

$(BUILD)/%.o: src/%.c lv_conf.h | $(LVGL_DIR)/lvgl.h
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/lvgl/%.o: $(LVGL_DIR)/src/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(LVGL_CFLAGS) -c $< -o $@

check-abi:
	@bad=$$($(READELF) --dyn-syms -W $(BIN) | grep -o '@GLIBC_[0-9.]*' | sort -u | \
		awk -F'[_.]' '$$2 > 2 || ($$2 == 2 && $$3 > 22)'); \
	if [ -n "$$bad" ]; then \
		echo "*** $(BIN) needs glibc symbols the device (2.22) does not have:" $$bad; \
		echo "*** add a .symver to $(PLAYER_DIR)/src/system/core/glibc_compat.h"; \
		rm -f $(BIN); exit 1; \
	fi; echo "check-abi: nothing beyond glibc 2.22"

# ---------------------------------------------------------------------------
# The simulator, built with the PC's own gcc. SDL2 (libsdl2-dev) gives it a
# window; without it only the headless screenshot mode is there.

HOST_CC ?= gcc
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
HAVE_SDL := $(if $(SDL_LIBS),1,0)
HOST_BUILD_DIR = build/host-$(MODEL)
HOST_BIN = $(HOST_BUILD_DIR)/sonix_recovery_sim
HOST_FT = $(CURDIR)/build/host-freetype/install
HOST_CFLAGS_BASE = -O1 -g -Wall -MMD -MP -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 -DSIM_HAVE_SDL=$(HAVE_SDL) \
				   -I$(CURDIR) -I$(LVGL_DIR)/.. -I$(LVGL_DIR) -I$(HOST_FT)/include/freetype2 \
				   -DLV_CONF_INCLUDE_SIMPLE=1 $(SDL_CFLAGS)
HOST_CFLAGS = $(HOST_CFLAGS_BASE) -Wextra -Wno-unused-parameter -Wno-format-truncation $(MODEL_DEF) \
			  -DSONIX_RECOVERY_VERSION='"$(VERSION) (simulator)"' \
			  -DSIM_FONT_FILE='"$(PLAYER_DIR)/assets/fonts/default.otf"'
HOST_SRCS = $(filter-out $(HW_SRCS),$(ALL_SRCS))
HOST_OBJS = $(HOST_SRCS:src/%.c=$(HOST_BUILD_DIR)/%.o)
# LVGL's objects depend on whether SDL is in, not on the model.
HOST_LVGL_DIR = build/host-lvgl-sdl$(HAVE_SDL)
HOST_LVGL_OBJS = $(LVGL_SRCS:$(LVGL_DIR)/src/%.c=$(HOST_LVGL_DIR)/%.o)

host: $(HOST_BIN)

$(HOST_FT)/lib/libfreetype.a:
	@echo "building FreeType for the host in build/host-freetype"
	@rm -rf build/host-freetype && mkdir -p build/host-freetype/obj
	@cd build/host-freetype/obj && sh $(FT_SRC)/configure --prefix=$(HOST_FT) --disable-shared --enable-static \
		--with-zlib=no --with-bzip2=no --with-png=no --with-harfbuzz=no --with-brotli=no >../configure.log 2>&1 && \
		$(MAKE) >../make.log 2>&1 && $(MAKE) install >../install.log 2>&1 || \
		{ echo "FreeType host build failed, see build/host-freetype/*.log" >&2; exit 1; }

$(HOST_OBJS) $(HOST_LVGL_OBJS): | $(HOST_FT)/lib/libfreetype.a

$(HOST_BIN): $(HOST_OBJS) $(HOST_LVGL_OBJS)
	@echo "link $@"
	@$(HOST_CC) -o $@ $^ $(HOST_FT)/lib/libfreetype.a $(SDL_LIBS) -lz -lpthread -lm
	@echo "built $@ ($(MODEL), SDL window: $(if $(filter 1,$(HAVE_SDL)),yes,no - install libsdl2-dev))"

$(HOST_BUILD_DIR)/%.o: src/%.c lv_conf.h | $(LVGL_DIR)/lvgl.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(HOST_LVGL_DIR)/%.o: $(LVGL_DIR)/src/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(HOST_CFLAGS_BASE) -w -c $< -o $@

shots: $(HOST_BIN)
	rm -rf build/shots-$(MODEL)
	cd $(HOST_BUILD_DIR) && ./sonix_recovery_sim --shots $(CURDIR)/build/shots-$(MODEL)

clean:
	rm -rf build

-include $(APP_OBJS:.o=.d) $(LVGL_OBJS:.o=.d) $(HOST_OBJS:.o=.d) $(HOST_LVGL_OBJS:.o=.d)
