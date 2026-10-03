# Belt on an emulated Alchemy Lab, on the Mac.
#
#   make -C emu            build/belt-emu
#   ./emu/build/belt-emu   window + mic in / speakers out (see emu/README.md)
#
# The firmware (src/belt_alchemy.cpp), the engine and the SDK framework are
# compiled UNCHANGED; only the board (emu/include/alchemy/hw/alchemy_lab_v2.h),
# libDaisy's surface (emu/include/daisy_seed.h ...) and the USB-host /
# picker modules (emu/src/emu_stubs.cpp) are replaced.

ROOT        := ..
ALCHEMY_DIR ?= $(ROOT)/lib/alchemy-sdk
# worktrees have empty submodules; fall back to the main checkout's
ifeq ($(wildcard $(ALCHEMY_DIR)/framework),)
ALCHEMY_DIR := $(shell git -C $(ROOT) rev-parse --path-format=absolute --git-common-dir)/../lib/alchemy-sdk
endif
LIBDAISY_DIR := $(ALCHEMY_DIR)/../libDaisy
FATFS       := $(LIBDAISY_DIR)/Middlewares/Third_Party/FatFs/src
BUILD       := build

SDL_CFLAGS  := $(shell sdl2-config --cflags)
SDL_LIBS    := $(shell sdl2-config --libs)

DEFS := -DALCHEMY_BOARD_V2 -DBELT_VERSION=\"emu\" -DBELT_GIT_HASH=\"$(shell git -C $(ROOT) rev-parse --short HEAD)\" \
        -DALCHEMY_HOSTLINK_MAX_BODY=2048 -DLIBDAISY_FFCONF=\"$(abspath $(LIBDAISY_DIR))/src/sys/ffconf.h\"

INCS := -Iinclude -Isrc -I$(ROOT)/src -I$(ROOT)/src/vendor \
        -I$(ALCHEMY_DIR)/framework/include -I$(ALCHEMY_DIR)/hardware/include \
        -I$(ALCHEMY_DIR)/hardware/alchemy-lab/v2/include \
        -I$(ALCHEMY_DIR)/tests/host -Iinclude/fatfs_conf -I$(FATFS) \
        -I$(ALCHEMY_DIR)

CXXFLAGS := -std=gnu++17 -O2 -g -w $(DEFS) $(INCS) $(SDL_CFLAGS)
CFLAGS   := -std=gnu11 -O2 -g -w $(DEFS) $(INCS) -ffast-math -fno-finite-math-only

SDK_SRC := \
  framework/src/surface/control_loop.cpp framework/src/surface/cv_matrix.cpp \
  framework/src/surface/led_render.cpp framework/src/surface/pager.cpp \
  framework/src/surface/preset_gesture_ui.cpp framework/src/surface/settings.cpp \
  framework/src/surface/virtual_knob.cpp framework/src/surface/button_bank.cpp \
  framework/src/led/panel.cpp framework/src/led/ring_frame.cpp framework/src/led/perf_renderer.cpp \
  framework/src/control/param_lock_manager.cpp framework/src/control/musical_clock.cpp \
  framework/src/control/cv_router.cpp framework/src/control/cv_edge.cpp \
  framework/src/host_link/host_link.cpp framework/src/host_link/descriptor.cpp framework/src/host_link/host.cpp \
  framework/src/host_link/describe.cpp framework/src/host_link/json_check.cpp \
  framework/src/storage/sd_card.cpp framework/src/storage/fs_extension.cpp \
  hardware/alchemy-lab/v2/src/alchemy_lab_v2_layout.cpp \
  hardware/alchemy-lab/v2/src/v2_calibration.cpp \
  tests/host/ram_diskio.cpp
SDK_ANIMS := $(wildcard $(ALCHEMY_DIR)/framework/src/led/anims/*.cpp)

FW_CXX := $(ROOT)/src/belt_alchemy.cpp $(ROOT)/src/usb_shared.cpp
FW_C   := $(ROOT)/src/belt_core_alchemy.c $(ROOT)/src/versio_alloc.c $(ROOT)/src/punch_fx.c
EMU_CXX := src/emu_board.cpp src/emu_stubs.cpp src/presets_emu.cpp src/emu_main.cpp src/emu_ui.cpp src/emu_script.cpp src/emu_font.cpp
EXTRA_C := $(FATFS)/ff.c $(FATFS)/option/ccsbcs.c

objname = $(BUILD)/obj/$(subst /,_,$(subst ..,up,$(1))).o

ALL_CXX := $(addprefix $(ALCHEMY_DIR)/,$(SDK_SRC)) $(SDK_ANIMS) $(EMU_CXX) $(ROOT)/src/usb_shared.cpp
ALL_C   := $(FW_C) $(EXTRA_C)

OBJS := $(foreach f,$(ALL_CXX),$(call objname,$(f))) \
        $(foreach f,$(ALL_C),$(call objname,$(f))) \
        $(BUILD)/obj/firmware.o

all: $(BUILD)/belt-emu

$(BUILD)/belt-emu: $(OBJS)
	$(CXX) $(OBJS) $(SDL_LIBS) -o $@

# The firmware's own TU: its main() becomes firmware_main(), ELF sections go.
$(BUILD)/obj/firmware.o: $(ROOT)/src/belt_alchemy.cpp $(wildcard include/*.h include/*/*.h include/*/*/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -include include/emu_prefix.h -Dmain=firmware_main -c $< -o $@

define CXX_RULE
$(call objname,$(1)): $(1) $(wildcard include/*.h include/*/*.h include/*/*/*.h src/*.h)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) -c $$< -o $$@
endef
define C_RULE
$(call objname,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) -c $$< -o $$@
endef
$(foreach f,$(ALL_CXX),$(eval $(call CXX_RULE,$(f))))
$(foreach f,$(ALL_C),$(eval $(call C_RULE,$(f))))

clean:
	rm -rf $(BUILD)

.PHONY: all clean test

# Every tests/*.emu script, headless, presets in RAM. Real time: ~1 min.
test: $(BUILD)/belt-emu
	@fail=0; for t in tests/*.emu; do \
	  echo "== $$t"; \
	  out=$$($(BUILD)/belt-emu --headless --flash none --script $$t 2>&1) || fail=1; \
	  echo "$$out" | grep -E 'PASS|FAIL|passed|FAILED|\?\?' || true; \
	done; exit $$fail
.PHONY: test
