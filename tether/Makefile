# =============================================================================
# TETHER -- MIDI to CV for the Hermetic Modular Alchemy Lab V2: a USB-MIDI
# device on the front USB-C (the Lab is the host) drives two pitch + gate
# voices, clock, reset and triggers. Lives beside Belt in this repo and
# shares its USB hub driver, SD firmware picker and linker script (../src).
#
#   make                -- build firmware -> build/tether_alchemy.bin
#   make program-dfu    -- flash over USB (module in DFU mode first)
#   make test           -- native engine tests (no hardware)
#   make clean
#
# Build `make libdaisy` once from the repo root first.
# =============================================================================

TARGET = tether_alchemy

BOARD ?= v2
ifneq ($(BOARD),v2)
$(error TETHER is V2 only: it drives J3-J10 as outputs)
endif

ALCHEMY_DIR  = ../lib/alchemy-sdk
LIBDAISY_DIR = ../lib/libDaisy

CPP_SOURCES = \
    src/tether_alchemy.cpp \
    src/tether_engine.cpp \
    src/tether_usbh.cpp \
    ../src/picker.cpp

C_SOURCES = \
    ../src/usbh_hub_midi.c

CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))

C_INCLUDES += \
    -Isrc \
    -I../src \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include

C_DEFS += -DALCHEMY_BOARD_V2

TETHER_VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
TETHER_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
C_DEFS += -DTETHER_VERSION=\"$(TETHER_VERSION)\" -DTETHER_GIT_HASH=\"$(TETHER_GIT_HASH)\"

# Runs from SRAM under the Daisy bootloader, like every Alchemy Lab firmware;
# FatFS compiled in for the SD picker.
USE_FATFS    = 1
APP_TYPE     = BOOT_SRAM
LDSCRIPT     = ../src/belt_alchemy.lds
CPP_STANDARD = -std=gnu++17
OPT          = -O2

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT)

.DEFAULT_GOAL := all

HASH_STAMP := $(BUILD_DIR)/.hash-$(TETHER_GIT_HASH)
ifeq ($(wildcard $(HASH_STAMP)),)
_HASH_GUARD := $(shell rm -f $(BUILD_DIR)/tether_alchemy.o $(BUILD_DIR)/tether_alchemy.d $(BUILD_DIR)/.hash-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(HASH_STAMP))
endif

.PHONY: test
test:
	$(MAKE) -C test
