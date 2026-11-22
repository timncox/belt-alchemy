/*
 * midi_host -- the front USB-C as a USB host for one MIDI device, or (0.2.0)
 * as a USB-MIDI device named TETHER for a computer, an iPhone or an iPad:
 * the port takes turns until one of them shows up (usb_role.h). As a host:
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
uint8_t  Stage();                /* 0 nothing, 1 enumerating, 2 failed / not MIDI, 3 MIDI ready,
                                    4 a computer has it as a MIDI device */
/* (0.3.1, 0.3.2) While Stage() is 2 for a device straight on the port: how
 * far its last try got. 1 it never answered, 2 lost taking its address or
 * giving its descriptors, 3 SET_CONFIGURATION failed, 4 configured but no
 * MIDI found or the MIDI class wouldn't start. 0 otherwise. */
uint8_t  FailWhere();
/* (0.3.2) The direct path's log, for /alchemy/tether_usb.txt: Report's lines,
 * then each step of the last tries, the device and its configuration. */
int      Trace(char* buf, int cap);
uint32_t TraceSerial();          /* changes when a try fails or a class starts: time to write */
uint32_t RxCount();              /* packets received */
/* (0.3.0) USB-MIDI event packets back to the computer, iPhone or iPad that
 * has the port as a MIDI device (stage 4): sent over the next polls. False
 * when nothing has the port that way or the last ones aren't out yet. */
bool     Send(const uint8_t (*pk)[4], int n);
uint32_t Dropped();              /* packets the queue had no room for */
int      Report(char* buf, int cap);   /* a few lines for the HostLink log */
} // namespace midihost
