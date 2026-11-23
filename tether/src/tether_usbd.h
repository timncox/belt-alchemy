/*
 * tether_usbd -- the front USB-C as TETHER's own USB device (0.5.0): one
 * configuration with USB audio both ways and the MIDI TETHER has had since
 * 0.2.0 (usb_audio_desc.h has the descriptors, usb_audio.h the streams).
 *
 * It takes the place of libDaisy's CDC class in MIDI mode, on libDaisy's
 * device core, device handle and low-level driver, so Mac mode (HostLink over
 * libDaisy's CDC class) is untouched. Module only; the emulator has its own
 * midihost (emu/tether_emu_midi.cpp).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace usbaudio
{
struct Link;
}

namespace tusbd
{
using RxFn = void (*)(uint8_t* buf, uint32_t len);   /* MIDI from the computer, as received */

void Start(RxFn rx);         /* the port as the device (the product string set first, in HS_Desc) */
void Stop();
bool Configured();           /* a host has set the configuration */
bool Addressed();            /* ... or at least given an address */
/* up to 64 bytes of USB-MIDI event packets to the host, copied; false while
 * the last are still going or nothing has the port */
bool MidiSend(const uint8_t* p, int len);
usbaudio::Link& Link();      /* the streams' rings */
int  Report(char* buf, int cap);   /* a line for the HostLink log */
} // namespace tusbd
