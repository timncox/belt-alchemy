/*
 * midi_host -- the front USB-C as a USB host for one MIDI device: plugged
 * straight in (libDaisy's MIDI class) or behind a hub (../src/usbh_hub_midi,
 * Belt's driver; a powered hub also charges what is plugged into it). Every
 * USB-MIDI event packet received is queued for the audio thread.
 *
 * tether_usbh.cpp on the module; emu/tether_emu_midi.cpp on the emulator,
 * where the script's `midi` lines arrive the same way.
 */
#pragma once
#include <stdint.h>

namespace midihost
{
void     Init();                 /* start the host (Belt waits ~2 s after boot first) */
void     Poll(uint32_t now_ms);  /* from the control loop's poll */
bool     Pop(uint32_t* pkt);     /* the next packet: cable|CIN, status, data 1, data 2 */
uint8_t  Stage();                /* 0 nothing, 1 enumerating, 2 failed / not MIDI, 3 MIDI ready */
uint32_t RxCount();              /* packets received */
uint32_t Dropped();              /* packets the queue had no room for */
int      Report(char* buf, int cap);   /* a few lines for the HostLink log */
} // namespace midihost
