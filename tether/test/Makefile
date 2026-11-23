# Native tests for TETHER's engine -- run on the laptop, no hardware.
#   make -C tether/test      (or `make test` in tether/)

CXX     ?= c++
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -I../src
BUILD    = ../build/test

.PHONY: test clean

test: $(BUILD)/test_engine $(BUILD)/test_cv_in $(BUILD)/test_midi_pick $(BUILD)/test_usb_role $(BUILD)/test_edges $(BUILD)/test_direct
	./$(BUILD)/test_engine
	./$(BUILD)/test_cv_in
	./$(BUILD)/test_midi_pick
	./$(BUILD)/test_usb_role
	./$(BUILD)/test_edges
	./$(BUILD)/test_direct

$(BUILD)/test_cv_in: test_cv_in.cpp ../src/cv_in.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_cv_in.cpp -o $@ -lm

$(BUILD)/test_edges: test_edges.cpp ../src/mcp_edges.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_edges.cpp -o $@

$(BUILD)/test_usb_role: test_usb_role.cpp ../src/usb_role.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_usb_role.cpp -o $@

$(BUILD)/test_midi_pick: test_midi_pick.cpp ../src/midi_itf_pick.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_midi_pick.cpp -o $@

$(BUILD)/test_engine: test_engine.cpp ../src/tether_engine.cpp ../src/tether_engine.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_engine.cpp ../src/tether_engine.cpp -o $@ -lm

clean:
	rm -rf $(BUILD)

# 0.3.1: ST's host core itself (libDaisy's copy) and Belt's hub driver, on a
# pretend port (usbh_sim/sim.cpp), around usbh_direct.h.
STUSB  = ../../lib/alchemy-sdk/vendor/libDaisy/Middlewares/ST/STM32_USB_Host_Library
SIM_C  = $(STUSB)/Core/Src/usbh_core.c $(STUSB)/Core/Src/usbh_ctlreq.c $(STUSB)/Core/Src/usbh_ioreq.c \
         $(STUSB)/Core/Src/usbh_pipes.c $(STUSB)/Class/MIDI/Src/usbh_midi.c ../../src/usbh_hub_midi.c
SIM_I  = -Iusbh_sim -I$(STUSB)/Core/Inc -I$(STUSB)/Class/MIDI/Inc -I../../src -I../src -I.
SIM_O  = $(patsubst %.c,$(BUILD)/sim/%.o,$(notdir $(SIM_C)))

$(BUILD)/sim/%.o: $(STUSB)/Core/Src/%.c
	@mkdir -p $(BUILD)/sim
	$(CC) -std=c11 -O1 -g -w $(SIM_I) -c $< -o $@
$(BUILD)/sim/%.o: $(STUSB)/Class/MIDI/Src/%.c
	@mkdir -p $(BUILD)/sim
	$(CC) -std=c11 -O1 -g -w $(SIM_I) -c $< -o $@
$(BUILD)/sim/%.o: ../../src/%.c
	@mkdir -p $(BUILD)/sim
	$(CC) -std=c11 -O1 -g -w $(SIM_I) -c $< -o $@

$(BUILD)/test_direct: usbh_sim/test_direct.cpp usbh_sim/sim.cpp usbh_sim/sim.h fm1_desc.inc ../src/usbh_direct.h ../src/midi_itf_pick.h $(SIM_O)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SIM_I) usbh_sim/test_direct.cpp usbh_sim/sim.cpp $(SIM_O) -o $@
