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

test: $(BUILD)/test_tuner $(BUILD)/test_chord $(BUILD)/test_chord_src
	./$(BUILD)/test_tuner
	./$(BUILD)/test_chord
	./$(BUILD)/test_chord_src

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

clean:
	rm -rf $(BUILD)
