# Native tests for TETHER's engine -- run on the laptop, no hardware.
#   make -C tether/test      (or `make test` in tether/)

CXX     ?= c++
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -I../src
BUILD    = ../build/test

.PHONY: test clean

test: $(BUILD)/test_engine $(BUILD)/test_midi_pick
	./$(BUILD)/test_engine
	./$(BUILD)/test_midi_pick

$(BUILD)/test_midi_pick: test_midi_pick.cpp ../src/midi_itf_pick.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_midi_pick.cpp -o $@

$(BUILD)/test_engine: test_engine.cpp ../src/tether_engine.cpp ../src/tether_engine.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_engine.cpp ../src/tether_engine.cpp -o $@ -lm

clean:
	rm -rf $(BUILD)
