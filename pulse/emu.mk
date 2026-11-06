# Pulse on the Alchemy Lab emulator (github.com/timncox/alchemy-lab, branch
# emu-custom):  make FW=custom FW_DIR=<this folder>  /  make test-custom ...
# The emulator stubs the shared SD picker, so its source is left out here.
EMU_TARGET       := pulse_alchemy
EMU_CPP_SOURCES  := src/pulse_alchemy.cpp src/pulse_engine.cpp
EMU_C_INCLUDES   := -Isrc -I../src
EMU_C_DEFS       := -DALCHEMY_BOARD_V2 -DPULSE_VERSION=\"emu\" -DPULSE_GIT_HASH=\"emu\"
EMU_ALCHEMY_DIR  := ../lib/alchemy-sdk
EMU_LIBDAISY_DIR := ../lib/libDaisy
EMU_LABELS       := RATIO|WAVE|WIDTH|LEVEL|OFFSET|PHASE
EMU_LABELS_ALT   := STEPS|FILLS|ROTATE|CHANCE|SWING|SLEW
