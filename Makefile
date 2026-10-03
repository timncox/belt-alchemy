# =============================================================================
# belt-alchemy — Belt, the live vocal processor, for the Hermetic Modular
# Alchemy Lab V2
#
# Standard Daisy workflow (libDaisy core Makefile underneath):
#   make libdaisy       — build lib/libDaisy once after cloning
#   make                — build firmware -> build/belt_alchemy.bin
#   make program-dfu    — flash over USB (module in DFU mode first)
#   make program-live   — reboot the running module over USB and flash it
#   make test           — native test suites (no hardware)
#   make clean          — remove the build tree
#
# Build flags:
#   BENCH_USB=1   HostLink on the Seed's micro-USB (bench bootloader setup)
#                 instead of the front USB-C. Rebuild without it before racking.
#
# Default goal is libDaisy's `all` (a build). Nothing here flashes unless a
# program-* target is named explicitly.
#
# Shared by copy with smack-alchemy and mark-alchemy: the picker, the SDRAM
# allocator, the linker script, tools/hostlink-fs.mjs and the shape of this
# file. A fix in one belongs in all three.
# =============================================================================

TARGET = belt_alchemy

# Alchemy Lab board revision: v1 | v2
BOARD ?= v2
ifeq ($(filter $(BOARD),v1 v2),)
$(error BOARD must be 'v1' or 'v2' (got '$(BOARD)'))
endif

ALCHEMY_DIR  = lib/alchemy-sdk
LIBDAISY_DIR = lib/libDaisy

# ── App sources ─────────────────────────────────────────────────────────────
CPP_SOURCES = \
    src/belt_alchemy.cpp \
    src/picker.cpp \
    src/launchpad.cpp \
    src/usb_shared.cpp \
    src/rear_midi.cpp

# belt_core_alchemy.c #includes vendor/belt_core.c with calloc/free
# redirected to SDRAM. Never list vendor/belt_core.c here as well —
# that would define every engine symbol twice.
C_SOURCES = \
    src/versio_alloc.c \
    src/belt_core_alchemy.c \
    src/usbh_hub_midi.c \
    src/usb_audio.c \
    src/usbd_ctlreq_uac.c \
    src/punch_fx.c \
    src/chord_src.c \
    src/vendor/seq.c

# src/vendor/seq.[ch] = seq-alchemy core/seq.[ch] at worktree-core b4453ea,
# byte-identical: the chord sequencer. seq-alchemy is the source of truth;
# re-copy, never edit here.

# ── Alchemy SDK, compiled straight from the submodule ───────────────────────
CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))

C_INCLUDES += \
    -Isrc \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include

ifeq ($(BOARD),v2)
C_DEFS += -DALCHEMY_BOARD_V2
endif

BELT_VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
BELT_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
C_DEFS += -DBELT_VERSION=\"$(BELT_VERSION)\" -DBELT_GIT_HASH=\"$(BELT_GIT_HASH)\"

ifeq ($(BENCH_USB),1)
C_DEFS += -DBELT_BENCH_USB
endif

# ── Daisy bootloader build (BOOT_SRAM) ──────────────────────────────────────
# The STM32H750 has 128 KB of internal flash; this app plus the SDK is well
# over that, so it runs from SRAM under the Daisy bootloader like every
# Alchemy Lab firmware.
# FatFS for the SD card. libdaisy.a carries ff.o but not the code-page
# helper (option/ccsbcs.c) that long-filename support links against;
# USE_FATFS=1 makes libDaisy's core Makefile compile the FatFS set into the
# app, which is how every SD-using Daisy firmware does it.
USE_FATFS = 1

APP_TYPE = BOOT_SRAM
# The SDK's alchemy_stm32h750ib_sram.lds plus one section: the SDK's SD-card
# code places FatFS work buffers in `.axi_bss`, which its own script does not
# define (see alchemy/storage/sd_card.h). Re-copy from the SDK and re-add the
# section when the submodule moves.
LDSCRIPT = src/belt_alchemy.lds

# The Alchemy SDK requires C++17 (libDaisy's default is gnu++14).
CPP_STANDARD = -std=gnu++17

# -O3, not -Os: the audio callback is the one budget that can kill this
# port -- seven TD-PSOLA voices on one Cortex-M7 is unmeasured until rack
# power. -ffast-math is applied to the engine's unit only, below.
OPT = -O3

# ── libDaisy core Makefile does the rest ────────────────────────────────────
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# The engine (seven TD-PSOLA voices) is the one budget that matters, and it
# overran: module 2 recorded a 100 % worst block on 2026-09-22 and the
# control loop starved (the Settings chord became unreliable). -ffast-math
# on this translation unit only -- libDaisy and the SDK keep strict IEEE.
# The engine has no isnan/isinf guards; -fno-finite-math-only keeps NaN
# comparisons honest anyway. test/Makefile builds the tuner test the same way.
BELT_ENGINE_FLAGS = -ffast-math -fno-finite-math-only
$(BUILD_DIR)/belt_core_alchemy.o: CFLAGS += $(BELT_ENGINE_FLAGS)

# Relink when the linker script changes. libDaisy's rules do not list it,
# and a stale link once left .belt_pool on top of the LED buffer's address.
$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT)

.DEFAULT_GOAL := all

# newlib-nano prints NOTHING for "%f" unless _printf_float is linked in, and
# the engine's detected_note / detected_freq getters format with %.2f.
override LDFLAGS += -u _printf_float

BOARD_STAMP := $(BUILD_DIR)/.board-$(BOARD)
ifeq ($(wildcard $(BOARD_STAMP)),)
_BOARD_GUARD := $(shell rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(BUILD_DIR)/*.lst $(BUILD_DIR)/.board-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(BOARD_STAMP))
endif

# The commit hash is a -D on every compile line, and make does not track
# flags, so the one object that bakes it in would stay stale across commits
# (smack-alchemy shipped a bin reporting the previous commit's hash). Retire
# that object whenever HEAD moves.
HASH_STAMP := $(BUILD_DIR)/.hash-$(BELT_GIT_HASH)
ifeq ($(wildcard $(HASH_STAMP)),)
_HASH_GUARD := $(shell rm -f $(BUILD_DIR)/belt_alchemy.o $(BUILD_DIR)/belt_alchemy.d $(BUILD_DIR)/.hash-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(HASH_STAMP))
endif

.PHONY: libdaisy
libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR)

# ── Flash without touching the module ───────────────────────────────────────
# HostLink reboots the running module into the bootloader over the same USB
# connection the web editor uses, then dfu-util (-w waits for the DFU device
# to enumerate) writes the app.
USBPID ?= df11

.PHONY: program-live
program-live: all
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)

# ── Native tests ────────────────────────────────────────────────────────────
.PHONY: test
test:
	$(MAKE) -C test
