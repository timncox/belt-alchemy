# Native tests for Pulse's engine — run on the laptop, no hardware.
#   make -C pulse/test      (or `make test` in pulse/)

CXX     ?= c++
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -I../src
BUILD    = ../build/test

.PHONY: test clean

test: $(BUILD)/test_engine
	./$(BUILD)/test_engine

$(BUILD)/test_engine: test_engine.cpp ../src/pulse_engine.cpp ../src/pulse_engine.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_engine.cpp ../src/pulse_engine.cpp -o $@ -lm

clean:
	rm -rf $(BUILD)
