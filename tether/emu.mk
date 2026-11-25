# TETHER on the Alchemy Lab emulator (github.com/timncox/alchemy-lab):
#   make FW=custom FW_DIR=<this folder>  /  make test-custom FW_DIR=<this folder>
# The emulator has no USB host: emu/tether_emu_midi.cpp stands in for
# src/tether_usbh.cpp and takes the script's `midi` lines (emulator 'midi'
# command, emu::midi). The SD picker is stubbed by the emulator.
EMU_TARGET       := tether_alchemy
EMU_CPP_SOURCES  := src/tether_alchemy.cpp src/tether_engine.cpp ../src/phone_ext.cpp
EMU_EXTRA_SRC    := emu/tether_emu_midi.cpp emu/tether_emu_picker.cpp
EMU_C_INCLUDES   := -Isrc -I../src
EMU_C_DEFS       := -DALCHEMY_BOARD_V2 -DTETHER_VERSION=\"emu\" -DTETHER_GIT_HASH=\"emu\"
EMU_ALCHEMY_DIR  := ../lib/alchemy-sdk
EMU_LIBDAISY_DIR := ../lib/libDaisy
EMU_LABELS       := PART|GATE|GLIDE|OCTAVE|TRANSPOSE|NOTES
EMU_LABELS_ALT   := J3|J4|J5|J6|J7|J8
