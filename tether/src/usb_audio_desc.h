/*
 * usb_audio_desc -- TETHER 0.5.0's USB device descriptors (the contract,
 * usb-audio-contract.md, shared with the FM-1 side). No SDK in it: the module
 * serves these (tether_usbd.cpp) and ../test/test_usb_audio.cpp checks them.
 *
 * One configuration, full speed, audio class 1.0 with no IAD, so a Mac or
 * an iPhone sees a class-compliant interface too:
 *
 *   0  AudioControl: USB streaming in (1) -> line out (2) for the FM-1's
 *      sound; line in (3) -> USB streaming out (4) for J1 / J2.
 *   1  AudioStreaming, FM-1 -> Lab: alt 0 none, alt 1 iso OUT 0x02 adaptive,
 *      192 bytes, 2 ch 16 bit 44.1 kHz.
 *   2  AudioStreaming, Lab -> FM-1: alt 0 none, alt 1 iso IN 0x83 async,
 *      192 bytes, the same format, no feedback endpoint.
 *   3  MIDIStreaming: TETHER's MIDI as before 0.5.0 (libDaisy's MIDI mode:
 *      one jack each way, bulk 0x01 / 0x81 of 64 bytes), now number 3.
 *
 * The device is class 0 (each interface says what it is), where libDaisy's
 * MIDI mode said 2 (CDC); vendor and product as before, release 5.00.
 */
#pragma once
#include <stdint.h>

namespace usbaudio
{

constexpr uint8_t kItfControl = 0, kItfOut = 1, kItfIn = 2, kItfMidi = 3, kNumItfs = 4;
constexpr uint8_t kEpAudioOut = 0x02, kEpAudioIn = 0x83, kEpMidiOut = 0x01, kEpMidiIn = 0x81;
constexpr uint16_t kMidiPacket = 64;

/* class-specific AudioControl: header 11, two input terminals 12, two output terminals 9 */
constexpr uint16_t kAcTotal = 11 + 12 + 9 + 12 + 9;
constexpr uint16_t kAsItf   = 9 + 9 + 7 + 11 + 9 + 7;   /* one streaming interface, both alts */
constexpr uint16_t kMidiItf = 9 + 7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5;
constexpr uint16_t kCfgTotal = 9 + 9 + kAcTotal + 2 * kAsItf + kMidiItf;
static_assert(kCfgTotal < 320, "the FM-1 host reads at most 320 bytes of configuration");

#define TETHER_LO(x) (uint8_t)((x) & 0xFF)
#define TETHER_HI(x) (uint8_t)(((x) >> 8) & 0xFF)

/* an AudioStreaming interface: number, the terminal it links to, its endpoint and its kind */
#define TETHER_AS_ITF(itf, link, ep, attr)                                                   \
    /* alt 0: no endpoints */                                                                \
    9, 0x04, itf, 0, 0, 0x01, 0x02, 0, 0,                                                    \
    /* alt 1: one iso endpoint */                                                            \
    9, 0x04, itf, 1, 1, 0x01, 0x02, 0, 0,                                                    \
    /* AS general: terminal link, delay 1, PCM */                                            \
    7, 0x24, 0x01, link, 1, 0x01, 0x00,                                                      \
    /* format type I: 2 channels, 2 bytes, 16 bits, one rate, 44100 */                       \
    11, 0x24, 0x02, 0x01, 2, 2, 16, 1, 0x44, 0xAC, 0x00,                                     \
    /* the endpoint (audio class: 9 bytes), 192 bytes, every frame */                        \
    9, 0x05, ep, attr, TETHER_LO(192), TETHER_HI(192), 1, 0, 0,                              \
    /* CS endpoint: general, no controls */                                                  \
    7, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00

alignas(4) static const uint8_t kConfigDesc[kCfgTotal] = {
    /* configuration: 4 interfaces, value 1, self powered, 160 mA (as before) */
    9, 0x02, TETHER_LO(kCfgTotal), TETHER_HI(kCfgTotal), kNumItfs, 1, 0, 0xC0, 0x50,

    /* 0 AudioControl */
    9, 0x04, kItfControl, 0, 0, 0x01, 0x01, 0, 0,
    11, 0x24, 0x01, 0x00, 0x01, TETHER_LO(kAcTotal), TETHER_HI(kAcTotal), 3, kItfOut, kItfIn, kItfMidi,
    /* IT 1: USB streaming, 2 ch L R */
    12, 0x24, 0x02, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0,
    /* OT 2: line connector, from 1 */
    9, 0x24, 0x03, 2, 0x03, 0x06, 0, 1, 0,
    /* IT 3: line connector, 2 ch L R */
    12, 0x24, 0x02, 3, 0x03, 0x06, 0, 2, 0x03, 0x00, 0, 0,
    /* OT 4: USB streaming, from 3 */
    9, 0x24, 0x03, 4, 0x01, 0x01, 0, 3, 0,

    /* 1 FM-1 -> Lab: iso OUT, adaptive */
    TETHER_AS_ITF(kItfOut, 1, kEpAudioOut, 0x09),
    /* 2 Lab -> FM-1: iso IN, asynchronous */
    TETHER_AS_ITF(kItfIn, 4, kEpAudioIn, 0x05),

    /* 3 MIDIStreaming, as libDaisy's MIDI mode served it (interface 1 then) */
    9, 0x04, kItfMidi, 0, 2, 0x01, 0x03, 0, 0,
    7, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
    6, 0x24, 0x02, 0x01, 0x01, 0x00,
    6, 0x24, 0x02, 0x02, 0x02, 0x00,
    9, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
    9, 0x24, 0x03, 0x02, 0x06, 0x01, 0x01, 0x01, 0x00,
    9, 0x05, kEpMidiOut, 0x02, TETHER_LO(kMidiPacket), TETHER_HI(kMidiPacket), 0, 0, 0,
    5, 0x25, 0x01, 0x01, 0x01,
    9, 0x05, kEpMidiIn, 0x02, TETHER_LO(kMidiPacket), TETHER_HI(kMidiPacket), 0, 0, 0,
    5, 0x25, 0x01, 0x01, 0x03,
};

/* the device: USB 2.0, class 0, EP0 64, ST's VID / PID as libDaisy's, release 5.00,
 * strings 1 2 3 (libDaisy's manufacturer, "TETHER", serial), one configuration */
alignas(4) static const uint8_t kDeviceDesc[18] = {
    18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
    TETHER_LO(1155), TETHER_HI(1155), TETHER_LO(22336), TETHER_HI(22336),
    0x00, 0x05, 1, 2, 3, 1,
};

#undef TETHER_AS_ITF
#undef TETHER_LO
#undef TETHER_HI

} // namespace usbaudio
