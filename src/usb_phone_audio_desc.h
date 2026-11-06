/*
 * usb_phone_audio_desc -- the configuration descriptor of the front USB-C's
 * "Phone+Audio" mode (USB port setting 4): ONE composite USB device that is
 * both the class-compliant audio interface of Audio mode (usb_audio.c) and
 * the USB MIDI port of Phone mode (HostLink inside SysEx, hlmidi.h).
 *
 * The use: an iPhone or iPad on the front USB-C sends its microphone into
 * Belt's input and hears Belt back over the same cable, while the app keeps
 * talking HostLink (STATE, SCREEN, PRESS, firmware switching) over MIDI.
 *
 * UNTESTED ON HARDWARE. It builds, and the host check
 * (alchemy-iphone-screen/firmware/tests/uac_composite_test.c) parses this
 * descriptor; nothing here has enumerated on a Mac, an iPhone or an iPad.
 *
 * USB Audio Class 1, full speed. The MIDIStreaming interface sits INSIDE the
 * audio function, as UAC1 / USB MIDI 1.0 allow: the AudioControl header's
 * baInterfaceNr lists both AudioStreaming interfaces and the MIDIStreaming
 * one, so no Interface Association Descriptor is needed.
 *
 *   itf  alt  class/sub      endpoint  type                     max packet
 *   0    0    01/01 AC       -         -                        -
 *   1    0    01/02 AS OUT   -         (zero bandwidth)         -
 *   1    1    01/02 AS OUT   0x01      iso ADAPTIVE (0x09)      294
 *   2    0    01/02 AS IN    -         (zero bandwidth)         -
 *   2    1    01/02 AS IN    0x82      iso SYNCHRONOUS (0x0D)   294
 *   3    0    01/03 MS       0x03      bulk OUT (phone -> Lab)  64
 *   3    0    01/03 MS       0x83      bulk IN  (Lab -> phone)  64
 *
 * Interfaces 0-2 and both isochronous endpoints are byte for byte Audio
 * mode's (k_cfg in usb_audio.c), except the AC header, which grows by one
 * byte for the third baInterfaceNr. Interface 3 is libDaisy's USB MIDI
 * block (Phone mode, usbd_cdc.c USBD_MIDI_CfgDesc) with its endpoints moved
 * to 0x03/0x83 and its jack IDs renumbered 0x11-0x14, so no ID in the whole
 * function repeats an AudioControl terminal's (1, 3, 4, 6).
 *
 * FIFO RAM (OTG_HS core in FS mode, 4 KB = 1024 words, set in this order by
 * UAC_StartWithMidi; Audio mode sets the first four the same way):
 *   RX   256 words  1024 B  shared by every OUT endpoint: two 294-byte iso
 *                           packets (2 x 75 words with status) plus bulk
 *                           OUT and setups; ST's minimum for this set is
 *                           13 + 74 + 2 x 3 + 1 = 94 words
 *   TX0   64 words   256 B  control
 *   TX1   16 words    64 B  unused (the old feedback EP; HAL's 16-word
 *                           minimum for an unused FIFO below a used one)
 *   TX2  256 words  1024 B  EP 0x82, two 294-byte iso IN packets
 *   TX3   64 words   256 B  EP 0x83, four 64-byte bulk packets: one whole
 *                           HostLink SysEx chunk (224 B) loads at once
 *   total 656 of 1024 words; 368 words (1472 B) left over.
 *
 * Device descriptor: VID 0x0483, PID 0x5730 as Audio mode, bcdDevice 2.01
 * (Audio mode is 1.01), so a host that caches by VID/PID/bcdDevice never
 * reuses the 3-interface layout for this one. Product string "Alchemy Lab",
 * as Phone mode, which is the MIDI device's name on iOS and macOS.
 *
 * Plain C, no HAL: usb_audio.c includes it, and so does the host check.
 */
#ifndef USB_PHONE_AUDIO_DESC_H
#define USB_PHONE_AUDIO_DESC_H

#include <stdint.h>

/* ---- format: must equal usb_audio.c's (checked there) -------------------- */
#define PA_RATE        48000u
#define PA_CH          2u
#define PA_SUBFRAME    3u
#define PA_PKT_MAX     ((PA_RATE / 1000u + 1u) * PA_CH * PA_SUBFRAME)   /* 294 */

/* ---- interfaces and endpoints -------------------------------------------- */
#define PA_ITF_AC      0u
#define PA_ITF_OUT     1u
#define PA_ITF_IN      2u
#define PA_ITF_MS      3u
#define PA_NUM_ITF     4u

#define PA_EP_AUDIO_OUT 0x01u   /* iso, adaptive                           */
#define PA_EP_AUDIO_IN  0x82u   /* iso, synchronous                        */
#define PA_EP_MIDI_OUT  0x03u   /* bulk, phone -> Lab                      */
#define PA_EP_MIDI_IN   0x83u   /* bulk, Lab -> phone                      */
#define PA_MIDI_PKT     64u

/* MIDI jack IDs, unique across the whole audio function. */
#define PA_JACK_IN_EMB  0x11u   /* fed by EP 0x03                          */
#define PA_JACK_IN_EXT  0x12u
#define PA_JACK_OUT_EMB 0x13u   /* feeds EP 0x83, from PA_JACK_IN_EXT      */
#define PA_JACK_OUT_EXT 0x14u   /* from PA_JACK_IN_EMB                     */

#define PA_BCD_DEVICE  0x0201u

/* ---- FIFO split, 32-bit words (see the header comment) ------------------- */
#define PA_FIFO_RX     0x100u
#define PA_FIFO_TX0    0x40u
#define PA_FIFO_TX1    0x10u
#define PA_FIFO_TX2    0x100u
#define PA_FIFO_TX3    0x40u
#define PA_FIFO_TOTAL  (PA_FIFO_RX + PA_FIFO_TX0 + PA_FIFO_TX1 + PA_FIFO_TX2 + PA_FIFO_TX3)
#define PA_FIFO_RAM    1024u
_Static_assert(PA_FIFO_TOTAL <= PA_FIFO_RAM, "OTG_HS FIFO RAM is 1024 words");
_Static_assert(PA_FIFO_TX3 * 4u >= PA_MIDI_PKT, "TX3 holds a bulk packet");
_Static_assert(PA_FIFO_TX2 * 4u >= 2u * PA_PKT_MAX, "TX2 holds two iso packets");

/* ---- lengths --------------------------------------------------------------- */
#define PA_AC_CS_LEN   (11u + 12u + 9u + 12u + 9u)                     /* 53 */
#define PA_AS_LEN      (9u + 9u + 7u + 11u + 9u + 7u)                  /* 52 */
#define PA_MS_CS_LEN   (7u + 6u + 6u + 9u + 9u + 9u + 5u + 9u + 5u)     /* 65 */
#define PA_MS_LEN      (9u + PA_MS_CS_LEN)                             /* 74 */
#define PA_CFG_LEN     (9u + 9u + PA_AC_CS_LEN + 2u * PA_AS_LEN + PA_MS_LEN) /* 249 */

#define PA_LO(x) (uint8_t)((x) & 0xFFu)
#define PA_HI(x) (uint8_t)(((x) >> 8) & 0xFFu)

#define PA_FORMAT_I                                                          \
    0x0B, 0x24, 0x02, 0x01, PA_CH, PA_SUBFRAME, 24, 0x01,                    \
    PA_LO(PA_RATE), PA_HI(PA_RATE), (uint8_t)(PA_RATE >> 16)

static const uint8_t k_cfg_phone_audio[PA_CFG_LEN] = {
    /* configuration */
    0x09, 0x02, PA_LO(PA_CFG_LEN), PA_HI(PA_CFG_LEN),
    PA_NUM_ITF,         /* interfaces                                 */
    0x01, 0x00,         /* value, string                              */
    0xC0,               /* self-powered (the rack)                    */
    0x32,               /* 100 mA                                     */

    /* interface 0: AudioControl */
    0x09, 0x04, PA_ITF_AC, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    /* AC header: 3 interfaces in the collection (2 AS + 1 MS) */
    0x0B, 0x24, 0x01, 0x00, 0x01, PA_LO(PA_AC_CS_LEN), PA_HI(PA_AC_CS_LEN), 0x03,
    PA_ITF_OUT, PA_ITF_IN, PA_ITF_MS,
    /* IT 1: USB streaming, stereo */
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, PA_CH, 0x03, 0x00, 0x00, 0x00,
    /* OT 3: line connector (J9/J10), from IT 1 */
    0x09, 0x24, 0x03, 0x03, 0x03, 0x06, 0x00, 0x01, 0x00,
    /* IT 4: line connector (J1/J2), stereo */
    0x0C, 0x24, 0x02, 0x04, 0x03, 0x06, 0x00, PA_CH, 0x03, 0x00, 0x00, 0x00,
    /* OT 6: USB streaming, from IT 4 */
    0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x00, 0x04, 0x00,

    /* interface 1 alt 0: OUT, zero bandwidth */
    0x09, 0x04, PA_ITF_OUT, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* interface 1 alt 1: OUT */
    0x09, 0x04, PA_ITF_OUT, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    /* AS general: terminal 1, PCM */
    0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00,
    PA_FORMAT_I,
    /* EP 0x01: isochronous, adaptive (usb_audio.c says why) */
    0x09, 0x05, PA_EP_AUDIO_OUT, 0x09, PA_LO(PA_PKT_MAX), PA_HI(PA_PKT_MAX),
    0x01, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    /* interface 2 alt 0: IN, zero bandwidth */
    0x09, 0x04, PA_ITF_IN, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* interface 2 alt 1: IN */
    0x09, 0x04, PA_ITF_IN, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    /* AS general: terminal 6, PCM */
    0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00,
    PA_FORMAT_I,
    /* EP 0x82: isochronous, synchronous */
    0x09, 0x05, PA_EP_AUDIO_IN, 0x0D, PA_LO(PA_PKT_MAX), PA_HI(PA_PKT_MAX),
    0x01, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    /* interface 3: MIDIStreaming, two bulk endpoints */
    0x09, 0x04, PA_ITF_MS, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    /* MS header, MIDI 1.00, total of the class-specific block */
    0x07, 0x24, 0x01, 0x00, 0x01, PA_LO(PA_MS_CS_LEN), PA_HI(PA_MS_CS_LEN),
    /* MIDI IN jacks: embedded, external */
    0x06, 0x24, 0x02, 0x01, PA_JACK_IN_EMB, 0x00,
    0x06, 0x24, 0x02, 0x02, PA_JACK_IN_EXT, 0x00,
    /* MIDI OUT jacks: embedded (from the external IN), external (from the
     * embedded IN) */
    0x09, 0x24, 0x03, 0x01, PA_JACK_OUT_EMB, 0x01, PA_JACK_IN_EXT, 0x01, 0x00,
    0x09, 0x24, 0x03, 0x02, PA_JACK_OUT_EXT, 0x01, PA_JACK_IN_EMB, 0x01, 0x00,
    /* EP 0x03 bulk OUT, 64; carries the embedded IN jack */
    0x09, 0x05, PA_EP_MIDI_OUT, 0x02, PA_LO(PA_MIDI_PKT), PA_HI(PA_MIDI_PKT),
    0x00, 0x00, 0x00,
    0x05, 0x25, 0x01, 0x01, PA_JACK_IN_EMB,
    /* EP 0x83 bulk IN, 64; carries the embedded OUT jack */
    0x09, 0x05, PA_EP_MIDI_IN, 0x02, PA_LO(PA_MIDI_PKT), PA_HI(PA_MIDI_PKT),
    0x00, 0x00, 0x00,
    0x05, 0x25, 0x01, 0x01, PA_JACK_OUT_EMB,
};
_Static_assert(sizeof k_cfg_phone_audio == PA_CFG_LEN, "Phone+Audio descriptor length");
_Static_assert(PA_CFG_LEN == 249u, "UAC1 2-in/2-out + USB MIDI is 249 bytes");

#endif
