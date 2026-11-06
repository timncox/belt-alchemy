# =============================================================================
# Pulse — master clock and eight clocked CV outputs for the Hermetic Modular
# Alchemy Lab V2. Lives beside Belt in this repo and shares its libraries,
# SD firmware picker and linker script (../src).
#
#   make                — build firmware -> build/pulse_alchemy.bin
#   make program-dfu    — flash over USB (module in DFU mode first)
#   make program-live   — reboot the running module over USB and flash it
#   make test           — native engine tests (no hardware)
#   make clean
#
# Build `make libdaisy` once from the repo root first.
# =============================================================================

TARGET = pulse_alchemy

BOARD ?= v2
ifneq ($(BOARD),v2)
$(error Pulse is V2 only: it drives J3-J10 as outputs)
endif

ALCHEMY_DIR  = ../lib/alchemy-sdk
LIBDAISY_DIR = ../lib/libDaisy

CPP_SOURCES = \
    src/pulse_alchemy.cpp \
    src/pulse_engine.cpp \
    ../src/picker.cpp

CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))

C_INCLUDES += \
    -Isrc \
    -I../src \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include

C_DEFS += -DALCHEMY_BOARD_V2

PULSE_VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
PULSE_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
C_DEFS += -DPULSE_VERSION=\"$(PULSE_VERSION)\" -DPULSE_GIT_HASH=\"$(PULSE_GIT_HASH)\"

# Runs from SRAM under the Daisy bootloader, like every Alchemy Lab firmware;
# FatFS compiled in for the SD picker (see ../Makefile for the why of each).
USE_FATFS    = 1
APP_TYPE     = BOOT_SRAM
LDSCRIPT     = ../src/belt_alchemy.lds
CPP_STANDARD = -std=gnu++17
OPT          = -O2

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT)

.DEFAULT_GOAL := all

HASH_STAMP := $(BUILD_DIR)/.hash-$(PULSE_GIT_HASH)
ifeq ($(wildcard $(HASH_STAMP)),)
_HASH_GUARD := $(shell rm -f $(BUILD_DIR)/pulse_alchemy.o $(BUILD_DIR)/pulse_alchemy.d $(BUILD_DIR)/.hash-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(HASH_STAMP))
endif

USBPID ?= df11

.PHONY: program-live
program-live: all
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)

.PHONY: test
test:
	$(MAKE) -C test
