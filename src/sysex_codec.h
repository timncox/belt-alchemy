/*
 * sysex_codec -- HostLink's bytes inside MIDI SysEx, for the front USB-C's
 * Phone role (sysex_transport.h). An iPhone app can reach a USB-MIDI device
 * through CoreMIDI but not a CDC serial port, so in Phone mode the module
 * enumerates as USB-MIDI and the same COBS/CRC HostLink frames ride inside
 * SysEx unchanged. No re-framing: a dropped or torn message only costs a
 * CRC failure, and the host retries as it does over CDC.
 *
 *   F0 7D 41 4C 01 <packed bytes> F7
 *      |  'A''L' version
 *      non-commercial manufacturer ID
 *
 * Packing is 7 bytes into 8: a byte holding the seven high bits (bit i =
 * bit 7 of byte i), then the seven bytes with bit 7 cleared. A short last
 * group packs only the bytes it has. Header-only and hardware-free so the
 * native tests and the web/iOS side can share one definition.
 *
 * Shared by copy (belt-alchemy first; meant for the SDK fork).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace sysex
{

constexpr uint8_t kHeader[5]   = {0xF0, 0x7D, 0x41, 0x4C, 0x01};
constexpr size_t  kHeaderBytes = sizeof kHeader;
/* Raw bytes per message: 256 packs to 293, a 299-byte SysEx, which
 * libDaisy's MidiUsbTransport turns into 400 bytes of USB-MIDI packets --
 * well inside its 1 KB transmit buffer. */
constexpr size_t kMaxRaw = 256;

/* Packed size of n raw bytes (without header / F7). */
constexpr size_t PackedSize(size_t n) { return n + (n + 6u) / 7u; }

/* Largest whole message for kMaxRaw raw bytes. */
constexpr size_t kMaxMessage = kHeaderBytes + PackedSize(kMaxRaw) + 1u;

/* Build a whole message (header, packed data, F7) into out; returns its
 * length. out must hold kHeaderBytes + PackedSize(n) + 1 bytes. */
inline size_t Encode(const uint8_t* src, size_t n, uint8_t* out)
{
    size_t o = 0;
    for (size_t i = 0; i < kHeaderBytes; i++) out[o++] = kHeader[i];
    for (size_t g = 0; g < n; g += 7u)
    {
        const size_t k   = n - g < 7u ? n - g : 7u;
        uint8_t      msb = 0;
        for (size_t i = 0; i < k; i++)
            if (src[g + i] & 0x80u) msb = (uint8_t)(msb | (1u << i));
        out[o++] = msb;
        for (size_t i = 0; i < k; i++) out[o++] = (uint8_t)(src[g + i] & 0x7Fu);
    }
    out[o++] = 0xF7;
    return o;
}

/* Streaming decoder: feed every received MIDI byte; decoded payload bytes
 * go to emit(byte, ctx). Messages without our header are ignored whole;
 * real-time bytes (F8-FF) may interleave anywhere and are skipped. */
class Decoder
{
  public:
    using Emit = void (*)(uint8_t b, void* ctx);

    void Feed(uint8_t b, Emit emit, void* ctx)
    {
        if (b >= 0xF8u) return;                 /* real-time: transparent */
        if (b == 0xF0u) { state_ = 1; return; } /* header byte 0 matched */
        if (b == 0xF7u) { Flush(emit, ctx); state_ = 0; return; }
        if (b & 0x80u) { state_ = 0; return; }  /* any other status aborts */
        if (state_ == 0) return;                /* not in our message */
        if (state_ < (int)kHeaderBytes)
        {
            state_ = b == kHeader[state_] ? state_ + 1 : 0;
            if (state_ == (int)kHeaderBytes) { have_ = 0; }
            return;
        }
        group_[have_++] = b;
        if (have_ == 8u) Flush(emit, ctx);
    }

  private:
    void Flush(Emit emit, void* ctx)
    {
        if (state_ == (int)kHeaderBytes && have_ > 1u)
        {
            const uint8_t msb = group_[0];
            for (size_t i = 1; i < have_; i++)
                emit((uint8_t)(group_[i] | (((msb >> (i - 1u)) & 1u) << 7)), ctx);
        }
        have_ = 0;
    }

    int     state_ = 0;   /* 0 idle, 1..4 header bytes matched, 5 data */
    uint8_t group_[8] = {};
    size_t  have_     = 0;
};

} // namespace sysex
