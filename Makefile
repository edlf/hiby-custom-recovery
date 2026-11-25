# Zee's Recovery.
#
#   make                 MODEL=r1 (default)  -> build/r1/zees_recovery
#   make MODEL=r3proii                       -> build/r3proii/zees_recovery
#   make host [MODEL=..]                     -> build/host-<model>/zees_recovery_sim
#                                               (the PC simulator, see src/sim_platform.c)
#   make shots                               host build, then screenshots of every
#                                               screen into build/shots-<model>/
#   make toolchain                           only the cross toolchain (deps/)
#   make clean                               build/; make distclean also deps/
#
# Everything it builds with is fetched or built here, under deps/ (not in
# git), on first use:
#   deps/rockbox-toolchain/  the cross toolchain (GCC 9, glibc 2.27), built by
#                            rockboxdev/rockboxdev.sh: an hour or so, once.
#                            TOOLCHAIN_DIR=... uses one built elsewhere.
#   deps/lvgl/               LVGL $(LVGL_VER), cloned (the folder has to be
#                            called lvgl: sources include "lvgl/lvgl.h")
#   deps/freetype-target/    FreeType $(FT_VER), static, for the MiSans face
#                            (src/style.c); the simulator gets a host build of
#                            the same source in build/host-freetype/
# LVGL is built once into build/lvgl/ and shared by both models.

MODEL ?= r1

DEPS = $(CURDIR)/deps
LVGL_VER = 9.6.0
LVGL_DIR ?= $(DEPS)/lvgl
TOOLCHAIN_DIR ?= $(DEPS)/rockbox-toolchain
CROSS ?= $(TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-
FT_VER = 2.13.3
FT_TARBALL = $(DEPS)/freetype-$(FT_VER).tar.xz
FT_URL = https://download.savannah.gnu.org/releases/freetype/freetype-$(FT_VER).tar.xz
FT_URL_MIRROR = https://downloads.sourceforge.net/project/freetype/freetype2/$(FT_VER)/freetype-$(FT_VER).tar.xz
FT_SRC = $(DEPS)/freetype-$(FT_VER)
FT_TARGET = $(DEPS)/freetype-target
# The face the recovery draws with: MiSans Regular, bundled into the root fs
# by pack_recovery.sh and used by the simulator.
FONT = $(CURDIR)/assets/fonts/default.otf
CC = $(CROSS)gcc
STRIP = $(CROSS)strip
READELF = $(CROSS)readelf

# LVGL is cloned before the source list below is read (not for `make clean`).
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(LVGL_DIR)/lvgl.h),)
$(info cloning LVGL v$(LVGL_VER) into $(LVGL_DIR))
$(shell mkdir -p $(DEPS) && git clone -q --depth 1 -b v$(LVGL_VER) https://github.com/lvgl/lvgl.git $(LVGL_DIR) >&2)
endif
endif

ifeq ($(MODEL),r1)
MODEL_DEF = -DMODEL_R1
else ifeq ($(MODEL),r3proii)
MODEL_DEF = -DMODEL_R3PROII
else
$(error MODEL must be r1 or r3proii)
endif

VERSION := $(shell git -C $(CURDIR) describe --always --dirty 2>/dev/null || echo unknown)

# -march=mips32r2: the toolchain is configured without --with-arch.
# -mno-madd4: the X1600 has no fused multiply-add (madd.s/msub.s would trap).
# src/glibc_compat.h: the device has glibc 2.22, the toolchain 2.27; it pins
# the old symbol versions, and check-abi below catches one the device would
# not have.
CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -MMD -MP -D_FILE_OFFSET_BITS=64 \
		 -march=mips32r2 -mno-madd4 -ffunction-sections -fdata-sections \
		 -include $(CURDIR)/src/glibc_compat.h \
		 -I$(CURDIR) -I$(LVGL_DIR)/.. -I$(LVGL_DIR) -I$(FT_TARGET)/include/freetype2 -DLV_CONF_INCLUDE_SIMPLE=1
LVGL_CFLAGS = $(filter-out -Wextra -Wno-unused-parameter,$(CFLAGS)) -w
APP_CFLAGS = $(CFLAGS) $(MODEL_DEF) -DZEES_RECOVERY_VERSION='"$(VERSION)"'
LDFLAGS = -static-libgcc -Wl,--gc-sections
LDLIBS = $(FT_TARGET)/lib/libfreetype.a -lpthread -lm

BUILD = build/$(MODEL)
BIN = $(BUILD)/zees_recovery

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

.PHONY: all clean distclean check-abi host shots toolchain
all: $(BIN)

$(LVGL_DIR)/lvgl.h:
	@echo "no LVGL in $(LVGL_DIR) (the clone failed?)" >&2; exit 1

# The toolchain, once (rockboxdev.sh downloads and builds binutils, GCC and
# glibc; its scratch goes in build/rbdev/). Only a prerequisite when CC is
# that toolchain's own: a CROSS= or CC= pointing elsewhere is used as it is.
TOOLCHAIN_GCC = $(TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-gcc
TOOLCHAIN_DEP = $(filter $(TOOLCHAIN_GCC),$(CC))

toolchain: $(TOOLCHAIN_GCC)

$(TOOLCHAIN_GCC):
	@echo "building the cross toolchain into $(TOOLCHAIN_DIR) (once; this takes a while)"
	@mkdir -p build/rbdev
	cd build/rbdev && RBDEV_HOST_PREFIX=$(TOOLCHAIN_DIR) $(CURDIR)/rockboxdev/rockboxdev.sh

$(FT_TARBALL):
	@mkdir -p $(DEPS)
	wget -q -O $@.part $(FT_URL) || wget -q -O $@.part $(FT_URL_MIRROR)
	mv $@.part $@

$(FT_SRC)/configure: $(FT_TARBALL)
	rm -rf $(FT_SRC) && tar -C $(DEPS) -xf $(FT_TARBALL) && touch $@

# FreeType for the device, static (the root fs has no libfreetype.so), built
# out of tree so the host build can use the same source. configure wants the
# toolchain on PATH to find ar and ranlib under their triplet names.
$(FT_TARGET)/lib/libfreetype.a: $(FT_SRC)/configure | $(TOOLCHAIN_DEP)
	@echo "building FreeType $(FT_VER) for the device into $(FT_TARGET)"
	@rm -rf build/target-freetype && mkdir -p build/target-freetype
	@cd build/target-freetype && PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" sh $(FT_SRC)/configure \
		--host=mipsel-rockbox-linux-gnu --prefix=$(FT_TARGET) --disable-shared --enable-static \
		--with-zlib=no --with-bzip2=no --with-png=no --with-harfbuzz=no --with-brotli=no \
		CC=$(CC) CFLAGS="-O2 -march=mips32r2 -mplt" AR=$(CROSS)ar RANLIB=$(CROSS)ranlib \
		>configure.log 2>&1 && \
		PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) >make.log 2>&1 && \
		PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) install >install.log 2>&1 || \
		{ echo "FreeType device build failed, see build/target-freetype/*.log" >&2; exit 1; }

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
		echo "*** add a .symver to src/glibc_compat.h"; \
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
HOST_BIN = $(HOST_BUILD_DIR)/zees_recovery_sim
HOST_FT = $(CURDIR)/build/host-freetype/install
HOST_CFLAGS_BASE = -O1 -g -Wall -MMD -MP -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 -DSIM_HAVE_SDL=$(HAVE_SDL) \
				   -I$(CURDIR) -I$(LVGL_DIR)/.. -I$(LVGL_DIR) -I$(HOST_FT)/include/freetype2 \
				   -DLV_CONF_INCLUDE_SIMPLE=1 $(SDL_CFLAGS)
HOST_CFLAGS = $(HOST_CFLAGS_BASE) -Wextra -Wno-unused-parameter -Wno-format-truncation $(MODEL_DEF) \
			  -DZEES_RECOVERY_VERSION='"$(VERSION) (simulator)"' \
			  -DSIM_FONT_FILE='"$(FONT)"'
HOST_SRCS = $(filter-out $(HW_SRCS),$(ALL_SRCS))
HOST_OBJS = $(HOST_SRCS:src/%.c=$(HOST_BUILD_DIR)/%.o)
# LVGL's objects depend on whether SDL is in, not on the model.
HOST_LVGL_DIR = build/host-lvgl-sdl$(HAVE_SDL)
HOST_LVGL_OBJS = $(LVGL_SRCS:$(LVGL_DIR)/src/%.c=$(HOST_LVGL_DIR)/%.o)

host: $(HOST_BIN)

$(HOST_FT)/lib/libfreetype.a: $(FT_SRC)/configure
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
	cd $(HOST_BUILD_DIR) && ./zees_recovery_sim --shots $(CURDIR)/build/shots-$(MODEL)

# build/ only: deps/ (toolchain, LVGL, FreeType) stays, `make distclean` too.
clean:
	rm -rf build

distclean: clean
	rm -rf $(DEPS)

-include $(APP_OBJS:.o=.d) $(LVGL_OBJS:.o=.d) $(HOST_OBJS:.o=.d) $(HOST_LVGL_OBJS:.o=.d)
