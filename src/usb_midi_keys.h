/*
 * usb_midi_keys -- the hardware-free half of the Lab's USB host: what a
 * configuration descriptor offers, which role a device gets, and a plain
 * class-compliant USB-MIDI keyboard's packets turned into note messages.
 *
 * Shared by copy with mark-alchemy and smack-alchemy, with launchpad.{h,cpp}
 * and usbh_hub_midi.{h,c} (see launchpad.h for the version). No HAL, no
 * libDaisy: test/test_usb_keys.c compiles it on the laptop.
 */
#ifndef USB_MIDI_KEYS_H
#define USB_MIDI_KEYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- descriptors */

#define UMK_KIND_NONE   0 /* nothing this host can drive */
#define UMK_KIND_MIDI   1 /* Audio class, MIDIStreaming subclass (01/03), bulk */
#define UMK_KIND_XINPUT 2 /* vendor interface ff/5d/01, interrupt */

typedef struct
{
    uint8_t  kind;               /* UMK_KIND_* */
    uint8_t  iface;              /* bInterfaceNumber of the interface taken */
    uint8_t  in_ep, out_ep;      /* 0 = none (a MIDI device may be IN-only) */
    uint16_t in_size, out_size;  /* wMaxPacketSize */
    uint8_t  interval;           /* XInput: interrupt IN bInterval, ms (>= 1) */
} umk_cfg_t;

/* Scan a configuration descriptor (all of it, or as much as was read) for
 * the first MIDIStreaming interface's bulk endpoints -- its first alternate
 * setting, which USB-MIDI 2.0 devices keep as MIDI 1.0 -- or an XInput
 * interface's interrupt endpoints. Returns the kind found; *out is filled. */
int umk_parse_cfg(const uint8_t *cfg, uint16_t len, umk_cfg_t *out);

/* Whether libDaisy's own MIDI host class (the device plugged straight in, no
 * hub) can serve this configuration. The ST host core picks a class from the
 * FIRST interface's class only, keeps the first max_ifaces interface
 * descriptors (alternate settings count) with at most max_eps endpoints
 * each, and reads at most max_cfg bytes. libDaisy: 2, 2, 256. */
int umk_direct_ok(const uint8_t *cfg, uint16_t len, int max_ifaces, int max_eps,
                  uint16_t max_cfg);

/* ---------------------------------------------------------------- roles */

#define UMK_ROLE_NONE 0
#define UMK_ROLE_MINI 1 /* Novation Launchpad Mini MK3, 1235:0113 */
#define UMK_ROLE_XL   2 /* Novation Launch Control XL, 1235:0061 */
#define UMK_ROLE_PAD  3 /* any XInput gamepad */
#define UMK_ROLE_KEYS 4 /* any other USB-MIDI device: notes in, nothing out */

int umk_role(uint16_t vid, uint16_t pid, int kind);

/* ---------------------------------------------------------------- keyboard */

/* Per device: which notes it holds down, so an unplug can let them go. */
typedef struct
{
    uint32_t held[4];  /* bit n = MIDI note n */
    uint8_t  sustain;  /* CC64 >= 64 last seen */
} umk_keys_t;

typedef struct { uint8_t b[3]; } umk_msg_t;

void umk_keys_reset(umk_keys_t *k);

/* USB-MIDI 1.0 event packets (4 bytes: cable|CIN, status, data1, data2) ->
 * channel-1 messages: note on (90 n v), note off (80 n 00), sustain (B0 40
 * v). Omni: every cable and channel. A note-on with velocity 0 is a note-off;
 * a note-off for a note this device does not hold is dropped, and so is a
 * repeated note-on. Everything else (other CCs, SysEx, clock) is ignored.
 * Returns the number of messages written (at most cap). */
int umk_keys_rx(umk_keys_t *k, const uint8_t *buf, size_t len, umk_msg_t *out, int cap);

/* The device went away: a note-off for every held note, then sustain off if
 * the pedal was down. Clears the state. Returns messages written. */
int umk_keys_release_all(umk_keys_t *k, umk_msg_t *out, int cap);

int umk_keys_held_count(const umk_keys_t *k);

#ifdef __cplusplus
}
#endif

#endif
