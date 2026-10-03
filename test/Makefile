# Native tests — run on the laptop, no hardware.
#
#   make -C test        (or `make test` from the repo root)
#
# test_tuner links belt_core_alchemy.c — the same translation unit the
# firmware builds — so the SDRAM bump-allocator redirect is exercised for
# real, and the pool size is the firmware's own (versio_alloc.h). It then
# holds the one vendoring edit that matters, 44.1 -> 48 kHz, against a sine:
# a wrong sample rate reads every note 1.47 semitones off, and nothing on
# the bench can hear that until rack power. The engine's own suite lives
# upstream in ~/tim-os/belt (`make test`).

CC     ?= cc
CFLAGS  = -O2 -g -Wall -Wextra -I../src -I../src/vendor -ffast-math -fno-finite-math-only
BUILD   = ../build/test

.PHONY: test clean

test: $(BUILD)/test_tuner $(BUILD)/test_chord $(BUILD)/test_chord_src $(BUILD)/test_usb_keys $(BUILD)/test_hub_unplug rear_link
	./$(BUILD)/test_tuner
	./$(BUILD)/test_chord
	./$(BUILD)/test_chord_src
	./$(BUILD)/test_usb_keys
	./$(BUILD)/test_hub_unplug

# The back link end to end: seq-alchemy's own chord-out sender (another
# repo; skipped when it is not there) -> Belt's rear_midi.cpp + libDaisy's
# MidiParser -> Belt's engine. `make -C test rear_link EMU_OUT=x.emu` also
# writes the byte stream as an emulator script.
SEQ_CHORD_OUT ?= $(HOME)/tim-os/seq-alchemy/.claude/worktrees/chord-out/core
LIBDAISY_DIR  ?= $(HOME)/tim-os/belt-alchemy/lib/libDaisy
.PHONY: rear_link
ifneq ($(wildcard $(SEQ_CHORD_OUT)/chord_midi.c),)
rear_link: $(BUILD)/test_rear_link
	./$(BUILD)/test_rear_link $(EMU_OUT)
else
rear_link:
	@echo "test_rear_link: skipped ($(SEQ_CHORD_OUT)/chord_midi.c not found)"
endif

REAR_C = ../src/versio_alloc.c ../src/belt_core_alchemy.c ../src/vendor/seq.c $(SEQ_CHORD_OUT)/chord_midi.c
$(BUILD)/test_rear_link: test_rear_link.cpp ../src/rear_midi.cpp $(REAR_C)
	@mkdir -p $(BUILD)/rear
	cd $(BUILD)/rear && $(CC) -c $(addprefix $(CURDIR)/,$(filter ../%,$(REAR_C))) \
	    $(filter /%,$(REAR_C)) $(CFLAGS:-I../%=-I$(CURDIR)/../%)
	$(CXX) -std=c++17 -O2 -g -Wall -Istubs -I../src -I../src/vendor -idirafter $(LIBDAISY_DIR)/src -idirafter $(SEQ_CHORD_OUT) \
	    test_rear_link.cpp ../src/rear_midi.cpp $(LIBDAISY_DIR)/src/hid/midi_parser.cpp \
	    $(BUILD)/rear/*.o -o $@ -lm

$(BUILD)/test_tuner: test_tuner.c ../src/versio_alloc.c ../src/belt_core_alchemy.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ -lm

# Hide and Seek: held notes, LEAD 0, the firmware's own engine unit.
$(BUILD)/test_chord: test_chord.c ../src/versio_alloc.c ../src/belt_core_alchemy.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ -lm

# The chord sources' rules on seq-alchemy's own core, under ASan + UBSan.
$(BUILD)/test_chord_src: test_chord_src.c ../src/chord_src.c ../src/vendor/seq.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -Wextra -I../src -fsanitize=address,undefined -fno-omit-frame-pointer $^ -o $@ -lm

# The USB host's hardware-free half: descriptors, roles, keyboard packets.
$(BUILD)/test_usb_keys: test_usb_keys.c ../src/usb_midi_keys.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

# The real hub driver against a simulated hub (fake_usbh/ = ST host headers):
# unplug mid-chord, re-plug, full slots, rejected ports, the proven set-up.
$(BUILD)/test_hub_unplug: test_hub_unplug.c ../src/usbh_hub_midi.c ../src/usb_midi_keys.c fake_usbh/usbh_core.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DDMA_SECTION= -Ifake_usbh test_hub_unplug.c ../src/usbh_hub_midi.c ../src/usb_midi_keys.c -o $@

clean:
	rm -rf $(BUILD)
