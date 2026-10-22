/* sysex_codec: HostLink bytes in MIDI SysEx (the Phone role). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "sysex_codec.h"

static int g_fail = 0;
#define CHECK(c, msg)                                                   \
    do {                                                                \
        if (!(c)) { printf("FAIL %s (line %d)\n", msg, __LINE__); g_fail++; } \
    } while (0)

static std::vector<uint8_t> g_out;
static void Collect(uint8_t b, void*) { g_out.push_back(b); }

static void FeedAll(sysex::Decoder& d, const std::vector<uint8_t>& wire)
{
    for (uint8_t b : wire) d.Feed(b, Collect, nullptr);
}

static std::vector<uint8_t> Enc(const std::vector<uint8_t>& raw)
{
    std::vector<uint8_t> m(sysex::kHeaderBytes + sysex::PackedSize(raw.size()) + 1u);
    const size_t         n = sysex::Encode(raw.data(), raw.size(), m.data());
    m.resize(n);
    return m;
}

int main()
{
    srand(1234);

    /* Every length 0..kMaxRaw round-trips, and the wire is valid SysEx. */
    for (size_t n = 0; n <= sysex::kMaxRaw; n++)
    {
        std::vector<uint8_t> raw(n);
        for (auto& b : raw) b = (uint8_t)rand();
        const auto wire = Enc(raw);
        CHECK(wire.size() == sysex::kHeaderBytes + sysex::PackedSize(n) + 1u, "encoded length");
        CHECK(wire.size() <= sysex::kMaxMessage, "fits kMaxMessage");
        bool seven = true;
        for (size_t i = 1; i + 1 < wire.size(); i++) seven = seven && wire[i] < 0x80u;
        CHECK(wire.front() == 0xF0 && wire.back() == 0xF7 && seven, "F0 .. 7-bit .. F7");
        sysex::Decoder d;
        g_out.clear();
        FeedAll(d, wire);
        CHECK(g_out == raw, "round trip");
    }

    /* Back-to-back messages, with real-time bytes sprinkled through. */
    {
        std::vector<uint8_t> a(100), b(37), wire;
        for (auto& x : a) x = (uint8_t)rand();
        for (auto& x : b) x = (uint8_t)rand();
        for (const auto* raw : {&a, &b})
            for (uint8_t x : Enc(*raw))
            {
                wire.push_back(x);
                if (rand() % 5 == 0) wire.push_back(0xF8);   /* clock */
            }
        sysex::Decoder d;
        g_out.clear();
        FeedAll(d, wire);
        std::vector<uint8_t> want = a;
        want.insert(want.end(), b.begin(), b.end());
        CHECK(g_out == want, "two messages + real-time");
    }

    /* Someone else's SysEx and channel messages are ignored; ours still lands. */
    {
        std::vector<uint8_t> raw = {0x00, 0xFF, 0x80, 0x7F, 0x01};
        std::vector<uint8_t> wire = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7,   /* identity req */
                                     0x90, 0x3C, 0x64,                     /* note on */
                                     0xF0, 0x7D, 0x41, 0x4D, 0x01, 0x05, 0xF7}; /* wrong tag */
        const auto ours = Enc(raw);
        wire.insert(wire.end(), ours.begin(), ours.end());
        sysex::Decoder d;
        g_out.clear();
        FeedAll(d, wire);
        CHECK(g_out == raw, "foreign traffic ignored");
    }

    /* A message cut off by a status byte emits nothing past the cut. */
    {
        std::vector<uint8_t> raw(20, 0xAA);
        auto wire = Enc(raw);
        wire.resize(sysex::kHeaderBytes + 8u + 3u);   /* one full group + 3 bytes */
        wire.push_back(0x90);                         /* note on aborts it */
        wire.push_back(0xF7);
        sysex::Decoder d;
        g_out.clear();
        FeedAll(d, wire);
        CHECK(g_out.size() == 7u, "only the complete group survives an abort");
    }

    printf("test_sysex: %s (%d failures)\n", g_fail ? "FAIL" : "pass", g_fail);
    return g_fail ? 1 : 0;
}
