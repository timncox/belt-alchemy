/*
 * MIDI in on the rear header: USART1 RX on header pin 7 (B5 = PB7 =
 * seed::D14), 31,250 baud, from another Lab's pin 8 (its USART1 TX). The
 * Lab-to-Lab cable crosses pin 8 to pin 7 both ways plus grounds and
 * NOTHING else: pins 1 / 11 / 15 are -12 V / +12 V / 3V3A, and a straight
 * 1:1 ribbon would join TX to TX and put the power rails through it. Never
 * a straight ribbon. (docs/alchemy-chord-vocoder-design.md, "Back link".)
 *
 * libDaisy's UartHandler receives into a DMA ring (DMA1_Stream5, unclaimed
 * by the SDK and this firmware); its callback, an interrupt, only copies
 * bytes into a small ring here. Poll() -- the control loop only -- feeds
 * them to libDaisy's MidiParser and hands back note on / note off.
 * libDaisy's MidiUartHandler is not used: it parses inside that interrupt
 * and queues 256 events of ~140 bytes (sysex buffers), ~36 KB of SRAM Belt
 * does not have to spare.
 *
 * v1 takes notes only, any channel. Clock (0xF8), start / stop (0xFA /
 * 0xFC), CCs and everything else are read and ignored.
 */
#pragma once
#include <cstdint>

namespace rearmidi {

/* Start listening. Call once, after the board's Init(). */
void Init();

/* Note callback: status 0x90 (note-on, vel > 0) or 0x80 (note-off, which
 * includes a note-on at velocity 0). */
using NoteFn = void (*)(uint8_t status, uint8_t note, uint8_t vel);

/* Parse everything received since the last call. Control loop only.
 * Restarts the receiver if a UART error stopped it. */
void Poll(NoteFn fn);

/* Bytes received since boot, and how many were dropped because the ring
 * was full (Poll() not keeping up). */
uint32_t Bytes();
uint32_t Dropped();

} // namespace rearmidi
