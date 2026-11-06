/*
 * launchpad_detail -- what launchpad.cpp (the USB host backend) and
 * launchpad_core.cpp (the queues, tables and framebuffers, and the remote
 * backend) share. Not for the firmware: it uses launchpad.h.
 *
 * The split keeps everything that is not USB hardware-free, so the remote
 * backend and the LED frame it returns build in the host tests.
 */
#pragma once
#include <stdint.h>

#include "launchpad.h"

namespace lpd
{

/* LED framebuffers, in CTRL's reply layout (CONTROLLERS.md):
 *   lp: grid 64 (row-major, y from the top), side 8 (top to bottom),
 *       top 8 (left to right), logo 1
 *   xl: knob LEDs 24 (row-major, rows top to bottom), button LEDs 16
 *       (upper row, then lower row) */
constexpr int kLpCells = 81;
constexpr int kLpSide0 = 64;
constexpr int kLpTop0  = 72;
constexpr int kLpLogo  = 80;
constexpr int kXlLeds  = 40;
constexpr int kXlKnobs = 24;

extern uint8_t lp_want[kLpCells];
extern uint8_t xl_want[kXlLeds];

/* Incoming controls, from either backend. Never block: a full queue drops. */
void LpPush(lp::Kind k, uint8_t x, uint8_t y, bool down);
void XlKnob(int k, uint8_t v);      /* k = row * 8 + col */
void XlFader(int col, uint8_t v);
void XlPush(uint8_t row, uint8_t col, bool down);

/* The gamepad's held mask (written by the USB receive callback). */
extern volatile uint32_t pad_buttons;
extern uint32_t          pad_reports;

/* The USB host backend's own view, defined in launchpad.cpp. */
bool UsbLpConnected();
bool UsbXlConnected();
bool UsbPadConnected();

} // namespace lpd
