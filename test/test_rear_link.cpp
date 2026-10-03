/*
 * The back link end to end, natively: seq-alchemy's chord-out sender
 * (its own core/chord_midi.c, branch chord-out, compiled as it is) turns
 * C - Am - F - G - C - stop into MIDI bytes, clock and all (0xFA, 0xF8 at
 * 24 PPQN, 0xFC); the bytes go into Belt's own receiver (src/rear_midi.cpp,
 * the UART swapped for test/stubs/per/uart.h) and libDaisy's MidiParser;
 * the notes go into Belt's own engine. Every millisecond, what Belt holds
 * must be exactly what the sender says it holds, and the engine's held
 * count must agree; stop must leave nothing held.
 *
 *   test_rear_link [script.emu]   also write the same byte stream, with its
 *                                 timing, as an emulator script
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "per/uart.h"
#include "rear_midi.h"

extern "C" {
#include "../src/versio_alloc.h"
#include "belt_core.h"
#include "chord_midi.h"   /* with its own seq.h (= src/vendor/seq.h) */
}

static int fails = 0;
#define CHECK(c, ...)                                                        \
    do {                                                                     \
        if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
                    std::printf(__VA_ARGS__); std::printf("\n"); fails++; }  \
    } while (0)

static belt_t* B;
static bool    g_held[128];
static int     g_ons = 0, g_offs = 0;

static void on_note(uint8_t st, uint8_t n, uint8_t v)
{
    g_held[n] = st == 0x90;
    st == 0x90 ? g_ons++ : g_offs++;
    const uint8_t m[3] = {st, n, v};
    belt_on_midi(B, m, 3, 0);   /* the firmware queues this to the callback */
}

static int engine_held(void)
{
    char buf[64];
    int  f[7] = {0};
    if (belt_get_param(B, "status", buf, sizeof buf) <= 0) return -1;
    std::sscanf(buf, "%d:%d:%d:%d:%d:%d:%d", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6]);
    return f[4];
}

static float fake_bpm(void) { return 120.0f; }

int main(int argc, char** argv)
{
    void* pool = std::malloc(VERSIO_POOL_BYTES);
    versio_alloc_init(pool, VERSIO_POOL_BYTES);
    host_api_v1_t host;
    std::memset(&host, 0, sizeof host);
    host.api_version = 1; host.sample_rate = BELT_SR; host.frames_per_block = 128;
    host.get_bpm = fake_bpm;
    B = belt_create(&host);
    assert(B && !versio_alloc_failed());
    belt_set_param(B, "midi_mode", "1");

    /* The sender, set up as its own C-Am-F-G test: C major, I vi IV V, a
     * bar each at 120 BPM, channel 1 -- here with the clock on. */
    static seq_t             S;
    static chord_midi_t      M;
    static chord_midi_ring_t R;
    seq_init(&S, 7);
    S.prog.root = 0; S.prog.scale = SEQ_SCALE_MAJOR; S.prog.tones = 3; S.prog.count = 4;
    const int8_t degs[4] = {0, 5, 3, 4};
    for (int i = 0; i < 4; i++) S.prog.chord[i].degree = degs[i];
    chord_midi_init(&M);
    M.mode = CHORD_MIDI_CLOCK;
    chord_midi_ring_init(&R);

    rearmidi::Init();

    struct Ev { uint32_t ms; std::vector<uint8_t> b; };
    std::vector<Ev>               evs;
    std::vector<std::vector<int>> chords;   /* each distinct held set, in order */
    int    mismatch = 0, engine_bad = 0, f8 = 0, fa = 0, fc = 0;
    int16_t silence[256] = {0};
    seq_out_t o;

    for (uint32_t ms = 0; ms < 8300; ms++)
    {
        if (ms == 50) seq_play(&S);
        if (ms == 8150) seq_stop(&S);
        seq_process(&S, ms * 1000u, &o);
        chord_midi_update(&M, &S, &R);
        uint8_t     x;
        Ev          ev{ms, {}};
        while (chord_midi_ring_pop(&R, &x))
        {
            ev.b.push_back(x);
            f8 += x == 0xF8; fa += x == 0xFA; fc += x == 0xFC;
        }
        if (!ev.b.empty())
        {
            uart_stub_inject(ev.b.data(), ev.b.size());
            evs.push_back(ev);
        }
        rearmidi::Poll(on_note);
        if (ms % 3 == 0) belt_process(B, silence, silence, 128);

        /* Belt holds exactly what the sender holds */
        bool want[128] = {false};
        for (int i = 0; i < M.n_held; i++) want[M.held[i]] = true;
        if (std::memcmp(want, g_held, sizeof want)) mismatch++;
        std::vector<int> now;
        for (int n = 0; n < 128; n++) if (g_held[n]) now.push_back(n);
        if (chords.empty() || chords.back() != now) chords.push_back(now);
        if (ms % 3 == 0 && engine_held() != (int)now.size()) engine_bad++;
    }

    const std::vector<std::vector<int>> want = {
        {}, {60, 64, 67}, {57, 60, 64}, {65, 69, 72}, {55, 59, 62}, {60, 64, 67}, {}};
    /* (Am keeps C E held, so C -> Am never passes through an empty set;
     * the others switch off first and briefly hold nothing, within the
     * same poll -- the same-poll sets never show, since Poll applies the
     * whole change before the check.) */
    std::vector<std::vector<int>> seen;
    for (auto& c : chords) if (seen.empty() || seen.back() != c) seen.push_back(c);
    std::printf("held sets:");
    for (auto& c : seen) { std::printf(" {"); for (int n : c) std::printf(" %d", n); std::printf(" }"); }
    std::printf("\nbytes %u (F8 %d, FA %d, FC %d), ons %d, offs %d, rx bytes %u dropped %u\n",
                (unsigned)rearmidi::Bytes(), f8, fa, fc, g_ons, g_offs,
                (unsigned)rearmidi::Bytes(), (unsigned)rearmidi::Dropped());
    CHECK(seen == want, "Belt held C, Am, F, G, C, then nothing");
    CHECK(mismatch == 0, "Belt's held notes differ from the sender's in %d ms", mismatch);
    CHECK(engine_bad == 0, "the engine's held count disagreed %d times", engine_bad);
    CHECK(f8 > 300 && fa == 1 && fc == 1, "the clock really was in the stream");
    /* C 3 + Am 1 (C, E stay held) + F 3 + G 3 + C 3 */
    CHECK(g_ons == g_offs && g_ons == 13, "every note-on answered (%d on, %d off)", g_ons, g_offs);
    CHECK(rearmidi::Dropped() == 0, "nothing dropped");
    CHECK(engine_held() == 0, "after stop the engine holds nothing");

    if (argc > 1)
    {
        /* The same stream for the emulator (alchemy-lab emu, tests/belt-chords):
         * set up as chords_midi.emu, then the bytes at their times, and a
         * tone check in the middle of each bar. */
        FILE* f = std::fopen(argv[1], "w");
        if (!f) return 2;
        std::fprintf(f,
            "# GENERATED by belt-alchemy test/test_rear_link.cpp from seq-alchemy\n"
            "# chord-out's own core/chord_midi.c (ea93d84): C - Am - F - G - C - stop at\n"
            "# 120 BPM, channel 1, with clock (FA, F8 at 24 PPQN, FC), into Belt's\n"
            "# rear-header receiver. Do not edit; regenerate.\n"
            "#!args --usb launchpad\n"
            "wait 4500\npot 1 0.04\npot 2 0.17\npot 3 0.0\npot 4 1.0\npot 5 0.9\n"
            "sing 220\nxl knob 1 1 0\nxl knob 1 2 0\nwait 500\n"
            "press b3\nwait 50\npress b2\nwait 2300\nrelease b2\nrelease b3\nwait 300\n"
            "tap b1\nwait 300\ntap b1\nwait 300\n"
            "pot 2 1.0\nwait 100\npot 2 0.5\nwait 100\npot 2 0.0\nwait 300\n"
            "tap b1\nwait 300\n"
            "pot 1 0.0\nwait 100\npot 1 0.5\nwait 100\npot 1 1.0\nwait 100\npot 1 0.875\nwait 300\n"
            "tap b3\nwait 600\n");
        struct Chk { uint32_t ms; const char* lines; };
        const Chk chk[] = {
            {1050, "expect tone 261.63 > 0.01\nexpect tone 329.63 > 0.01\nexpect tone 392.00 > 0.01\n"},
            {3050, "expect tone 220.00 > 0.01\nexpect tone 261.63 > 0.01\nexpect tone 392.00 < 0.006\n"},
            {5050, "expect tone 349.23 > 0.01\nexpect tone 440.00 > 0.01\nexpect tone 523.25 > 0.005\n"},
            {7050, "expect tone 196.00 > 0.01\nexpect tone 246.94 > 0.01\nexpect tone 293.66 > 0.01\n"},
            {8250, "mark\n"},
        };
        uint32_t t = 0;
        size_t   ci = 0;
        auto     to = [&](uint32_t ms) {
            if (ms > t) { std::fprintf(f, "wait %u\n", ms - t); t = ms; }
        };
        for (auto& e : evs)
        {
            while (ci < sizeof chk / sizeof chk[0] && chk[ci].ms <= e.ms)
            {
                to(chk[ci].ms);
                std::fputs(chk[ci].lines, f);
                ci++;
            }
            to(e.ms);
            std::fprintf(f, "uart");
            for (uint8_t x : e.b) std::fprintf(f, " %02x", x);
            std::fprintf(f, "\n");
        }
        for (; ci < sizeof chk / sizeof chk[0]; ci++) { to(chk[ci].ms); std::fputs(chk[ci].lines, f); }
        to(8700);
        std::fprintf(f, "expect rms < 0.003\nexpect booted\nexpect alive\nexpect finite\n");
        std::fclose(f);
        std::printf("wrote %s (%zu uart lines)\n", argv[1], evs.size());
    }

    std::printf("test_rear_link: %s (%d failures)\n", fails ? "FAIL" : "pass", fails);
    return fails ? 1 : 0;
}
