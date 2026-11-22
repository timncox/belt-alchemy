/*
 * Native tests for tether_engine -- no hardware. Runs the engine as the
 * firmware does (24-frame blocks at 48 kHz) and feeds it the USB-MIDI
 * packets PROTECTOR's PARTS setting sends.
 */
#include <cmath>
#include <cstdio>
#include <cstring>

#include "tether_engine.h"
#include "tether_sysex.h"

using namespace tether;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) g_pass++;                                                \
        else {                                                             \
            g_fail++;                                                      \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);  \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

static constexpr float kSr = 48000.0f;
static constexpr int   kBlock = 24;

/* PROTECTOR's packet: CIN | status << 8 | d1 << 16 | d2 << 24 */
static uint32_t pk(uint8_t st, uint8_t d1 = 0, uint8_t d2 = 0)
{
    return (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
}
static void on(Engine& e, int ch, int note, int vel = 100) { e.Packet(pk((uint8_t)(0x90 | (ch - 1)), (uint8_t)note, (uint8_t)vel)); }
static void off(Engine& e, int ch, int note) { e.Packet(pk((uint8_t)(0x80 | (ch - 1)), (uint8_t)note, 0)); }
static void rt(Engine& e, uint8_t st) { e.Packet(pk(st)); }

/* Run `ms`; count rising edges (through 2.5 V) of J3..J8 and the time any
 * jack spent high. */
struct Run { int rises[kJacks] = {}; int high_blocks[kJacks] = {}; int low_blocks[kJacks] = {}; };
static bool g_prev[kJacks];
static Run run(Engine& e, float ms, Run* acc = nullptr)
{
    Run   r;
    bool* hi = g_prev;   /* the level seen at the end of the last run: an edge a packet made counts */
    const int blocks = (int)lroundf(ms * kSr / 1000.0f / kBlock);
    for (int b = 0; b < blocks; b++)
    {
        e.Process(kBlock);
        for (int j = 0; j < kJacks; j++)
        {
            const bool h = e.JackVolts(j) > 2.5f;
            if (h && !hi[j]) r.rises[j]++;
            if (h) r.high_blocks[j]++; else r.low_blocks[j]++;
            hi[j] = h;
        }
    }
    if (acc) for (int j = 0; j < kJacks; j++) { acc->rises[j] += r.rises[j]; acc->high_blocks[j] += r.high_blocks[j]; acc->low_blocks[j] += r.low_blocks[j]; }
    return r;
}

/* The tests from 0.1 / 0.2 run on CLOCK, that version's layout. */
static Engine fresh(int layout = kLayoutClock)
{
    Engine e;
    e.Init(kSr);
    SetLayout(e.master.jack, layout);
    for (int j = 0; j < kJacks; j++) g_prev[j] = false;
    return e;
}

static bool near(float a, float b, float tol = 0.002f) { return std::fabs(a - b) < tol; }

static void test_defaults()
{
    Engine e;
    e.Init(kSr);
    CHECK(e.voice[0].chan == 4 && e.voice[1].chan == 5 && e.voice[2].chan == 6, "A BASS ch5, B LEAD ch6, C PAD ch7");
    CHECK(e.voice[2].note_mode == kLow, "the pad plays a chord's lowest note");
    /* BAND, the default from 0.3.0 */
    CHECK(LayoutOf(e.master.jack) == kLayoutBand, "BAND");
    CHECK(e.master.jack[0] == kFnGateA && e.master.jack[1] == kFnGateB, "J3 bass gate, J4 lead gate");
    CHECK(e.master.jack[2] == kFnTrig1 + 2 && e.master.jack[3] == kFnClk16, "J5 HATS, J6 16ths");
    CHECK(e.master.jack[4] == kFnTrig1 && e.master.jack[5] == kFnTrig1 + 1, "J7 KICK J8 SNARE");
    CHECK(e.master.jack[6] == kFnPitchA && e.master.jack[7] == kFnPitchB, "J9 bass pitch, J10 lead pitch");
    for (int j = 0; j < kJacks; j++) CHECK(near(e.JackVolts(j), 0.0f), "jack %d quiet at boot", j);
    CHECK(e.master.clk_ppqn == kPpqnDefault && kPpqnValues[e.master.clk_ppqn] == 4 && e.master.j2_mode == kJ2Reset,
          "J1 at 4 PPQN, J2 resets");
    /* CLOCK is 0.2.0's */
    uint8_t j[kJacks];
    SetLayout(j, kLayoutClock);
    const uint8_t old[kJacks] = {kFnGateA, kFnGateB, kFnClk16, kFnReset, kFnTrig1, kFnTrig1 + 1, kFnPitchA, kFnPitchB};
    CHECK(!memcmp(j, old, kJacks) && LayoutOf(j) == kLayoutClock, "CLOCK = 0.2.0's jacks");
    j[2] = kFnClk8;
    CHECK(LayoutOf(j) == -1 && !strcmp(LayoutName(-1), "CUSTOM"), "a jack changed by hand: CUSTOM");
}

/* BAND with PROTECTOR's parts: every output doing its job. */
static void test_band()
{
    Engine e = fresh(kLayoutBand);
    on(e, 1, 36, 127);  on(e, 2, 38, 127);  on(e, 3, 42, 127);   /* KICK SNARE HATS */
    on(e, 5, 36);                                  /* bass C2 */
    on(e, 6, 72);                                  /* lead C5 */
    on(e, 7, 64); on(e, 7, 60); on(e, 7, 67);      /* pad: a C chord, played out of order */
    run(e, 2);
    CHECK(e.JackVolts(4) == kGateV && e.JackVolts(5) == kGateV && e.JackVolts(2) == kGateV, "KICK J7, SNARE J8, HATS J5");
    CHECK(e.JackVolts(0) == kGateV, "J3 bass gate");
    CHECK(near(e.JackVolts(6), -2.0f), "J9 bass pitch -2 V (%f)", e.JackVolts(6));
    CHECK(near(e.JackVolts(7), 1.0f), "J10 lead pitch +1 V");
    CHECK(e.JackVolts(1) == kGateV, "J4 lead gate");
    CHECK(e.master.jack[3] == kFnClk16, "J6 16ths clock out");
    /* the pad's pitch and the lead's envelope, set by hand on J6 and J4 */
    e.master.jack[1] = kFnEnvB;
    e.master.jack[3] = kFnPitchC;
    CHECK(LayoutOf(e.master.jack) < 0, "changed by hand: CUSTOM");
    CHECK(near(e.JackVolts(3), 0.0f), "J6 pad pitch: the chord's lowest, C4 = 0 V (%f)", e.JackVolts(3));
    CHECK(e.JackVolts(1) > 0.5f && e.JackVolts(1) < kGateV, "J4 lead envelope rising (%f)", e.JackVolts(1));
    /* the lead's envelope: attack to 5 V, decay to sustain, release on note off */
    float peak = 0.0f;
    for (int b = 0; b < 40; b++) { e.Process(kBlock); peak = std::fmax(peak, e.JackVolts(1)); }
    off(e, 1, 36); off(e, 2, 38); off(e, 3, 42);
    run(e, 10);
    CHECK(e.JackVolts(4) < 2.5f, "drums down after their notes");
    run(e, 1500);
    const float sus = e.JackVolts(1);
    CHECK(peak > 4.9f, "attack reaches 5 V (%f)", peak);
    CHECK(near(sus, kGateV * e.voice[1].sus, 0.05f), "sustain at %.2f V (%f)", kGateV * e.voice[1].sus, sus);
    off(e, 6, 72);
    run(e, 50);
    CHECK(e.JackVolts(1) < sus && e.JackVolts(1) > 0.0f, "releasing");
    run(e, 1500);
    CHECK(e.JackVolts(1) < 0.01f, "released");
    /* a new lead note after a gap attacks again */
    on(e, 6, 74);
    run(e, 3);
    CHECK(e.JackVolts(1) > 0.0f, "again");
    /* the pad's calibration follows its pitch to J6 */
    e.master.cal_off[2] = 0.01f;
    CHECK(near(e.JackVolts(3), 0.01f + 0.0f * 1.0f + (0.0f - 1.0f) * 0.0f, 1e-4f), "J6 with C's calibration (%f)", e.JackVolts(3));
    e.SetRef(2);
    CHECK(near(e.JackVolts(3), 3.01f, 1e-4f) && near(e.JackVolts(6), 3.0f, 1e-4f), "the reference on every pitch jack");
}

/* ---- J1 clock, J2 reset (0.3.0) ---- */

struct Pulses   /* a block's worth of J1 / J2 samples */
{
    float j1[kBlock], j2[kBlock];
};
/* Run `ms` feeding J1 a 5 ms pulse every `period_ms` (0: none) starting at
 * `phase_ms`, and J2 a pulse at each time in `resets` (ms from now). */
static Run feed(Engine& e, float ms, float period_ms, float phase_ms = 0.0f, const float* resets = nullptr, int nres = 0,
                Run* acc = nullptr)
{
    Run   r;
    const int blocks = (int)lroundf(ms * kSr / 1000.0f / kBlock);
    for (int b = 0; b < blocks; b++)
    {
        Pulses p;
        for (int i = 0; i < kBlock; i++)
        {
            const float t = ((float)(b * kBlock + i)) * 1000.0f / kSr;   /* ms */
            float h = 0.0f;
            if (period_ms > 0.0f && t >= phase_ms && std::fmod(t - phase_ms, period_ms) < 5.0f) h = 1.0f;   /* 5 V */
            p.j1[i] = h;
            float g = 0.0f;
            for (int k = 0; k < nres; k++) if (t >= resets[k] && t < resets[k] + 5.0f) g = 1.0f;
            p.j2[i] = g;
        }
        e.Inputs(p.j1, p.j2, kBlock);
        e.Process(kBlock);
        for (int j = 0; j < kJacks; j++)
        {
            const bool h = e.JackVolts(j) > 2.5f;
            if (h && !g_prev[j]) r.rises[j]++;
            g_prev[j] = h;
        }
    }
    if (acc) for (int j = 0; j < kJacks; j++) acc->rises[j] += r.rises[j];
    return r;
}

static void test_j1_clock()
{
    /* CLOCK layout: J5 16ths, J6 reset. J1 at 4 PPQN, 120 BPM: a pulse every 125 ms. */
    Engine e = fresh();
    e.master.jack[0] = kFnClk24;
    e.master.jack[1] = kFnRun;
    Run r = feed(e, 2000, 125.0f);
    CHECK(r.rises[2] == 16, "J1 at 4 PPQN: 16 sixteenths on J5 in 2 s (%d)", r.rises[2]);
    CHECK(r.rises[0] >= 94 && r.rises[0] <= 96, "24 PPQN spread between the pulses (%d)", r.rises[0]);
    CHECK(r.rises[3] == 1, "a clock arriving starts it: RESET once (%d)", r.rises[3]);
    CHECK(e.Running() && e.JackVolts(1) == kGateV && e.ClockSeen() && e.J1Clock(), "RUN up, B2 sees the clock");
    /* the clock stops: so does the transport, after three of its periods */
    run(e, 300);
    CHECK(!e.Running() && !e.ClockSeen(), "lost clock: stopped");
    /* and starts again from the top with the next pulse */
    r = feed(e, 130, 125.0f);
    CHECK(r.rises[3] == 1 && r.rises[2] == 2, "restart from the top: RESET, 16ths (%d %d)", r.rises[3], r.rises[2]);

    /* other rates: 1 PPQN gives a 16th every quarter of a pulse */
    e = fresh();
    e.master.clk_ppqn = 0;
    r = feed(e, 2000, 500.0f);
    CHECK(r.rises[2] == 16, "1 PPQN: still 16 sixteenths (%d)", r.rises[2]);
    e = fresh();
    e.master.clk_ppqn = 5;   /* 24 */
    r = feed(e, 1000, 1000.0f / 48.0f);
    CHECK(r.rises[2] == 8, "24 PPQN in: 8 sixteenths a second (%d)", r.rises[2]);
}

static void test_j2_reset()
{
    Engine e = fresh();
    e.master.jack[0] = kFnBar;
    Run acc;
    feed(e, 1000, 125.0f, 0.0f, nullptr, 0, &acc);   /* two bars, almost */
    CHECK(acc.rises[0] == 1, "bar 1 (%d)", acc.rises[0]);
    /* J2 mid-bar, between pulses: the next J1 pulse is the top of the bar */
    const float rs[1] = {60.0f};
    Run r = feed(e, 200, 125.0f, 0.0f, rs, 1);
    CHECK(r.rises[3] == 1, "J2: RESET out (%d)", r.rises[3]);
    CHECK(r.rises[0] == 1, "and a new bar on the next pulse (%d)", r.rises[0]);
    /* J2 with a J1 pulse at the same moment: that pulse is beat one, one reset */
    e = fresh();
    e.master.jack[0] = kFnBar;
    feed(e, 600, 125.0f);
    const float rs2[1] = {250.0f};
    r = feed(e, 260, 125.0f, 0.0f, rs2, 1);
    CHECK(r.rises[3] == 1, "one RESET (%d)", r.rises[3]);
    CHECK(r.rises[0] == 1, "one bar, at the reset (%d)", r.rises[0]);
}

static void test_j2_start_stop()
{
    Engine e = fresh();
    e.master.j2_mode = kJ2StartStop;
    e.master.jack[0] = kFnRun;
    Run r = feed(e, 500, 125.0f);
    CHECK(r.rises[2] == 0 && !e.Running(), "START/STOP: the clock alone doesn't start it");
    const float go[1] = {10.0f};
    r = feed(e, 500, 125.0f, 0.0f, go, 1);
    /* the pulse at 0 ms came before J2 at 10: the next three count */
    CHECK(e.Running() && r.rises[3] == 1 && r.rises[2] == 3, "J2 starts it: RESET, 16ths (%d %d)", r.rises[3], r.rises[2]);
    r = feed(e, 500, 125.0f, 0.0f, go, 1);
    CHECK(!e.Running() && r.rises[2] <= 1, "J2 again stops it (%d)", r.rises[2]);
}

static void test_midi_wins()
{
    Engine e = fresh();
    rt(e, 0xFA);
    Run acc;
    /* MIDI clock at 120 BPM (a tick every 20.83 ms) with J1 at a different
     * tempo: J5 follows MIDI alone */
    for (int t = 0; t < 96; t++)
    {
        rt(e, 0xF8);
        feed(e, 1000.0f / 48.0f, 50.0f, 0.0f, nullptr, 0, &acc);
    }
    CHECK(acc.rises[2] == 16, "MIDI clock: 16 sixteenths in 2 s, J1 ignored (%d)", acc.rises[2]);
    CHECK(!e.J1Clock() && e.MidiClock(), "MIDI clock wins");
    /* J2 does nothing either */
    const float rs[1] = {5.0f};
    Run r = feed(e, 20, 0.0f, 0.0f, rs, 1);
    CHECK(r.rises[3] == 0, "J2 ignored under MIDI clock");
    /* MIDI clock gone for half a second: J1 takes over */
    r = feed(e, 2000, 125.0f);
    CHECK(e.J1Clock() && r.rises[2] >= 12, "J1 takes over once MIDI clock stops (%d)", r.rises[2]);
}

/* ---- settings over SysEx (Loadout) ---- */

static void sysex_in(Engine& e, const uint8_t* msg, int n)
{
    uint8_t pkts[80][4];
    const int np = sysex::Packets(msg, n, pkts, 80);
    for (int i = 0; i < np; i++)
        e.Packet((uint32_t)pkts[i][0] | (uint32_t)pkts[i][1] << 8 | (uint32_t)pkts[i][2] << 16 | (uint32_t)pkts[i][3] << 24);
}

static void test_sysex()
{
    Engine e = fresh(kLayoutBand);
    const uint8_t get[] = {0xF0, 0x7D, 0x54, 0x45, 0x01, 0xF7};
    sysex_in(e, get, sizeof get);
    uint8_t req[200];
    int     n = 0;
    CHECK(e.TakeSysex(req, sizeof req, &n) && n == 4, "GET handed over (%d)", n);
    CHECK(!e.TakeSysex(req, sizeof req, &n), "once");
    sysex::Request r;
    CHECK(sysex::Parse(req, n, &r) && r.cmd == sysex::kGet && !r.why, "parsed");

    /* the answer: version, then the state in halves; it reads back */
    uint8_t st[kStateBytes];
    Pack(e.master, e.voice, 0, st);
    uint8_t msg[sysex::kStateMsgBytes];
    const int len = sysex::State(st, "0.3.0", msg);
    CHECK(len == sysex::kStateMsgBytes && msg[0] == 0xF0 && msg[len - 1] == 0xF7 && msg[4] == sysex::kState, "STATE framed");
    bool clean = true;
    for (int i = 1; i < len - 1; i++) if (msg[i] & 0x80) clean = false;
    CHECK(clean, "7-bit inside");
    CHECK(!memcmp(msg + 5, "0.3.0\0\0\0", 8), "version");

    /* SET: CLOCK, voice C on channel 10, J1 at 24 PPQN */
    MasterParams m = e.master;
    VoiceParams  v[kVoices];
    for (int i = 0; i < kVoices; i++) v[i] = e.voice[i];
    SetLayout(m.jack, kLayoutClock);
    v[2].chan  = 9;
    m.clk_ppqn = 5;
    Pack(m, v, 2, st);
    uint8_t set[4 + 1 + 2 * kStateBytes + 2];
    int k = 0;
    set[k++] = 0xF0; set[k++] = 0x7D; set[k++] = 0x54; set[k++] = 0x45; set[k++] = sysex::kSet;
    for (int i = 0; i < kStateBytes; i++) { set[k++] = st[i] >> 4; set[k++] = st[i] & 15; }
    set[k++] = 0xF7;
    sysex_in(e, set, k);
    CHECK(e.TakeSysex(req, sizeof req, &n) && sysex::Parse(req, n, &r) && r.cmd == sysex::kSet && !r.why, "SET parsed");
    MasterParams m2;
    VoiceParams  v2[kVoices];
    uint8_t      sel;
    CHECK(Unpack(r.state, m2, v2, sel) && LayoutOf(m2.jack) == kLayoutClock && v2[2].chan == 9 && m2.clk_ppqn == 5 && sel == 2,
          "SET carries the whole state");
    /* a short one, a wrong check, an unknown command, LAYOUT */
    set[k - 2] = 0xF7;
    sysex_in(e, set, k - 1);
    CHECK(e.TakeSysex(req, sizeof req, &n) && sysex::Parse(req, n, &r) && r.why == sysex::kWhyLength, "short: refused");
    const uint8_t lay[] = {0xF0, 0x7D, 0x54, 0x45, 0x03, 0x01, 0xF7};
    sysex_in(e, lay, sizeof lay);
    CHECK(e.TakeSysex(req, sizeof req, &n) && sysex::Parse(req, n, &r) && r.cmd == sysex::kLayout && r.layout == 1, "LAYOUT CLOCK");
    const uint8_t bad[] = {0xF0, 0x7D, 0x54, 0x45, 0x03, 0x05, 0xF7};
    sysex_in(e, bad, sizeof bad);
    CHECK(e.TakeSysex(req, sizeof req, &n) && sysex::Parse(req, n, &r) && r.why == sysex::kWhyCommand, "no layout 5");
    /* someone else's SysEx, and notes around it, are left alone */
    const uint8_t other[] = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0xF7};
    sysex_in(e, other, sizeof other);
    CHECK(!e.TakeSysex(req, sizeof req, &n), "not ours: ignored");
    on(e, 5, 60);
    run(e, 2);
    CHECK(e.Gate(0), "notes still play after SysEx");
    uint8_t refused[8];
    CHECK(sysex::Refused(2, refused) == 7 && refused[5] == 2, "REFUSED");
}

static void test_note_gate_pitch()
{
    Engine e = fresh();
    on(e, 5, 48);
    CHECK(near(e.PitchVolts(0), -1.0f), "C3 on A = -1 V before the gate, got %f", e.PitchVolts(0));
    e.Process(kBlock);   /* 0.5 ms: still waiting */
    CHECK(e.JackVolts(0) == 0.0f, "gate waits 1 ms for the pitch");
    run(e, 2);
    CHECK(e.JackVolts(0) == kGateV, "gate A up after 1 ms");
    CHECK(e.JackVolts(1) == 0.0f, "B untouched");
    off(e, 5, 48);
    e.Process(kBlock);
    CHECK(e.JackVolts(0) == 0.0f, "gate down on note off");
    CHECK(near(e.PitchVolts(0), -1.0f), "pitch holds after release");
    on(e, 6, 72);
    run(e, 2);
    CHECK(near(e.PitchVolts(1), 1.0f) && e.JackVolts(1) == kGateV, "B: C5 = +1 V, gate");
    on(e, 3, 60);
    run(e, 2);
    CHECK(e.Note(0) == -1, "channel 3 not A's");
}

static void test_legato_slide()
{
    Engine e = fresh();
    on(e, 5, 48);
    run(e, 10);
    /* PROTECTOR's slide: the new note on before the old one's off */
    on(e, 5, 50);
    off(e, 5, 48);
    Run r = run(e, 20);
    CHECK(r.low_blocks[0] == 0, "LEGATO: the gate never drops on a slide (%d low blocks)", r.low_blocks[0]);
    CHECK(near(e.PitchVolts(0), (50 - 60) / 12.0f), "slid to D3");
    /* the same note slid to itself */
    on(e, 5, 50);
    off(e, 5, 50);
    r = run(e, 10);
    CHECK(r.low_blocks[0] == 0 && e.Gate(0), "same-note slide keeps the gate");
    off(e, 5, 50);
    run(e, 1);
    CHECK(!e.Gate(0), "and the last off ends it");
    /* plain steps: off then on retriggers */
    on(e, 5, 48);
    run(e, 10);
    off(e, 5, 48);
    on(e, 5, 48);
    r = run(e, 10);
    CHECK(r.rises[0] == 1, "off-then-on: a new gate (%d)", r.rises[0]);
    CHECK(r.low_blocks[0] >= 5, "off-then-on in one USB frame: down about 3 ms (%d blocks)", r.low_blocks[0]);
    /* a long rest is not stretched: 1 ms as usual */
    off(e, 5, 48);
    run(e, 10);
    on(e, 5, 48);
    r = run(e, 10);
    CHECK(r.low_blocks[0] <= 2, "after a rest the gate waits only 1 ms (%d blocks)", r.low_blocks[0]);
}

static void test_retrig_and_priority()
{
    Engine e = fresh();
    e.voice[0].note_mode = kRetrig;
    on(e, 5, 48);
    run(e, 10);
    on(e, 5, 50);
    Run r = run(e, 10);
    CHECK(r.rises[0] == 1 && r.low_blocks[0] >= 4, "RETRIG: overlap drops the gate ~3 ms (%d rises, %d low)", r.rises[0], r.low_blocks[0]);
    off(e, 5, 50);
    run(e, 10);
    CHECK(e.Note(0) == 48 && e.Gate(0), "back to the held note");
    off(e, 5, 48);
    run(e, 1);
    CHECK(!e.Gate(0), "all up: gate down");

    e.voice[0].note_mode = kLow;
    on(e, 5, 55);
    on(e, 5, 43);
    on(e, 5, 60);
    run(e, 5);
    CHECK(e.Note(0) == 43, "LOW holds the lowest (%d)", e.Note(0));
    off(e, 5, 43);
    run(e, 5);
    CHECK(e.Note(0) == 55, "then the next lowest");
    e.Panic();
    e.voice[0].note_mode = kHigh;
    on(e, 5, 55);
    on(e, 5, 43);
    run(e, 5);
    CHECK(e.Note(0) == 55, "HIGH holds the highest");
}

static void test_clock_reset_run()
{
    Engine e = fresh();
    e.master.jack[0] = kFnClk24;
    e.master.jack[1] = kFnClk4;
    e.master.jack[4] = kFnRun;
    e.master.jack[5] = kFnBar;
    rt(e, 0xFA);
    CHECK(e.JackVolts(3) == kGateV, "RESET trig on start");
    CHECK(e.JackVolts(4) == kGateV, "RUN up");
    Run acc;
    const float tick_ms = 60000.0f / 120.0f / 24.0f;   /* 20.83 ms */
    for (int t = 0; t < 96; t++)
    {
        rt(e, 0xF8);
        run(e, tick_ms, &acc);
    }
    CHECK(acc.rises[0] == 96, "CLK 24: every tick (%d)", acc.rises[0]);
    CHECK(acc.rises[2] == 16, "CLK 16: 16ths (%d)", acc.rises[2]);
    CHECK(acc.rises[1] == 4, "CLK 4: quarters (%d)", acc.rises[1]);
    CHECK(acc.rises[5] == 1, "BAR: once (%d)", acc.rises[5]);
    CHECK(acc.rises[3] == 1, "RESET once, at start (%d)", acc.rises[3]);
    rt(e, 0xFC);
    Run s;
    for (int t = 0; t < 24; t++) { rt(e, 0xF8); run(e, tick_ms, &s); }
    CHECK(s.rises[0] == 0 && s.rises[2] == 0, "stopped: the clock jacks rest");
    CHECK(e.JackVolts(4) == 0.0f, "RUN down");
    /* 300 BPM: 24 PPQN trigs stay shorter than half a tick */
    rt(e, 0xFA);
    const float fast = 60000.0f / 300.0f / 24.0f;   /* 8.3 ms */
    Run f;
    for (int t = 0; t < 48; t++) { rt(e, 0xF8); run(e, fast, &f); }
    CHECK(f.rises[0] == 48, "CLK 24 at 300 BPM: separate pulses (%d)", f.rises[0]);
}

static void test_clock_before_transport()
{
    Engine e = fresh();
    Run    r;
    for (int t = 0; t < 24; t++) { rt(e, 0xF8); run(e, 20.0f, &r); }
    CHECK(r.rises[2] == 4, "no start seen yet: the clock still comes through (%d)", r.rises[2]);
    CHECK(e.ClockSeen(), "clock seen");
}

static void test_drums_section_accent()
{
    Engine e = fresh();
    on(e, 1, 36, 127);
    CHECK(e.JackVolts(4) == kGateV && e.JackVolts(5) == 0.0f, "KICK on J7, 5 V at velocity 127");
    off(e, 1, 36);
    Run r = run(e, 10);
    CHECK(e.JackVolts(4) == 0.0f && r.high_blocks[4] >= 9 && r.high_blocks[4] <= 11, "a short note: 5 ms (%d blocks)", r.high_blocks[4]);
    on(e, 2, 38, 1);
    CHECK(near(e.JackVolts(5), 2.5f), "SNARE at velocity 1: the 2.5 V floor (%f)", e.JackVolts(5));
    on(e, 1, 36, 64);
    CHECK(near(e.JackVolts(4), 2.5f + 2.5f * 63.0f / 126.0f), "KICK at 64: half way (%f)", e.JackVolts(4));
    r = run(e, 30);
    CHECK(e.JackVolts(4) > 2.5f && r.low_blocks[4] == 0, "held: a gate until note off");
    on(e, 1, 36, 127);
    CHECK(e.JackVolts(4) == 0.0f, "a new hit while up: down first");
    run(e, 4);
    CHECK(e.JackVolts(4) == kGateV, "then up again, at its own height");
    off(e, 1, 36); off(e, 1, 36); off(e, 2, 38);
    run(e, 10);
    CHECK(e.JackVolts(4) == 0.0f && e.JackVolts(5) == 0.0f, "down at note off");
    e.master.drum_vel = 0;
    on(e, 2, 38, 1);
    CHECK(e.JackVolts(5) == kGateV, "velocity off: always 5 V");
    e.master.drum_vel = 0x02;
    e.master.drum_floor[1] = 0.0f;
    on(e, 2, 38, 1);
    run(e, 4);
    CHECK(near(e.JackVolts(5), 0.0f), "floor 0: velocity 1 is 0 V");
    off(e, 2, 38); run(e, 10);
    /* held: up for as long as the note (Ultra Perc reads the length) */
    e.master.drum_vel = 0;
    on(e, 1, 36, 100);
    run(e, 100);
    CHECK(e.JackVolts(4) == kGateV, "held gate: still up 100 ms on");
    off(e, 1, 36); run(e, 2);
    CHECK(e.JackVolts(4) == 0.0f, "held gate: down at the note off");
    /* the last note's off and the next note's on together: down 3 ms first */
    on(e, 1, 36, 100);
    run(e, 50);
    off(e, 1, 36); on(e, 1, 36, 100);
    CHECK(e.JackVolts(4) == 0.0f, "off and on together: down first");
    run(e, 4);
    CHECK(e.JackVolts(4) == kGateV, "then up: the module hears two hits");
    off(e, 1, 36); run(e, 10);
    /* a hit long after the last one: up at once */
    on(e, 1, 36, 100);
    CHECK(e.JackVolts(4) == kGateV, "after a rest: up at once, no gap");
    off(e, 1, 36); run(e, 10);
    /* TRIG: TRIG LENGTH whatever the note */
    e.master.drum_trig = 0x01;
    on(e, 1, 36, 100);
    run(e, 3);
    CHECK(e.JackVolts(4) == kGateV, "TRIG: up");
    run(e, 20);
    CHECK(e.JackVolts(4) == 0.0f, "TRIG: down after TRIG LENGTH though held");
    off(e, 1, 36); run(e, 2);
    e.master.drum_trig = 0;

    e.master.jack[2] = kFnSection;
    e.Packet(pk(0xBF, 85, 2));
    CHECK(near(e.JackVolts(2), 2.0f), "SECTION C = 2 V");
    CHECK(e.JackVolts(3) == 0.0f, "no reset on section by default");
    e.master.reset_on = kResetStartSection;
    e.Packet(pk(0xBF, 85, 3));
    CHECK(e.JackVolts(3) == kGateV, "RESET on section change when set");

    e.voice[0].gate_mode = kAccGate;
    on(e, 5, 48, 120);
    run(e, 2);
    CHECK(e.JackVolts(0) == kAccentV, "ACC GATE: accented = 5 V");
    off(e, 5, 48);
    on(e, 5, 48, 90);
    run(e, 4);
    CHECK(e.JackVolts(0) == kPlainAccV, "ACC GATE: plain = 3.5 V");
    e.voice[0].gate_mode = kVelGate;
    CHECK(near(e.JackVolts(0), 1.0f + 4.0f * 90.0f / 127.0f), "VEL GATE height, 1-5 V");
    e.master.jack[1] = kFnVelA;
    CHECK(near(e.JackVolts(1), 5.0f * 90.0f / 127.0f), "VEL A, 0-5 V");
    e.master.jack[1] = kFnAccA;
    CHECK(e.JackVolts(1) == 0.0f, "ACC A low on a plain note");
    off(e, 5, 48);
    on(e, 5, 50, 127);
    run(e, 4);
    CHECK(e.JackVolts(1) == kGateV, "ACC A high on an accent");

    e.voice[0].gate_mode = kTrig;
    off(e, 5, 50);
    run(e, 20);
    on(e, 5, 48);
    Run t = run(e, 20);
    CHECK(t.rises[0] == 1 && t.high_blocks[0] <= 11, "TRIG: 5 ms then down while held (%d rises, %d high)", t.rises[0], t.high_blocks[0]);
}

static void test_glide_oct_bend()
{
    Engine e = fresh();
    e.voice[0].glide = 0.3f;   /* tau ~90 ms */
    on(e, 5, 48);
    e.Process(kBlock);
    CHECK(near(e.PitchVolts(0), -1.0f), "LEGATO: a fresh note does not glide");
    on(e, 5, 60);
    off(e, 5, 48);
    run(e, 20);
    const float mid = e.PitchVolts(0);
    CHECK(mid > -0.95f && mid < -0.05f, "slide glides (%f after 20 ms)", mid);
    run(e, 1000);
    CHECK(near(e.PitchVolts(0), 0.0f), "and arrives");
    e.voice[0].octave = -2;
    e.voice[0].transpose = 7;
    off(e, 5, 60);
    e.voice[0].glide = 0.0f;
    on(e, 5, 60);
    run(e, 2);
    CHECK(near(e.PitchVolts(0), -2.0f + 7.0f / 12.0f), "OCTAVE -2, +7 semitones (%f)", e.PitchVolts(0));
    e.Packet(pk(0xE4, 0x7F, 0x7F));   /* bend up, range 2 */
    CHECK(near(e.PitchVolts(0), -2.0f + 9.0f / 12.0f, 0.003f), "bend +2 (%f)", e.PitchVolts(0));
    e.voice[1].chan = kChanAll;
    on(e, 11, 72);
    run(e, 2);
    CHECK(e.Note(1) == 72, "ALL: any channel");
    e.Packet(pk(0xBA, 123, 0));
    CHECK(!e.Gate(1) && e.Gate(0), "all notes off on ch11: B only");
}

static void test_cal()
{
    Engine e = fresh();
    e.SetRef(1);
    CHECK(near(e.PitchVolts(0), 1.0f) && near(e.PitchVolts(1), 1.0f), "REF LOW = 1 V");
    CHECK(e.JackVolts(0) == kGateV && e.JackVolts(1) == kGateV, "gates up to hear it");
    e.master.cal_off[0] = 0.012f;
    CHECK(near(e.PitchVolts(0), 1.012f, 1e-4f), "offset trims at 1 V");
    e.master.cal_scale[0] = 0.01f;
    CHECK(near(e.PitchVolts(0), 1.012f, 1e-4f), "scale does not move 1 V");
    e.SetRef(2);
    CHECK(near(e.PitchVolts(0), 3.032f, 1e-4f), "scale trims at 3 V (%f)", e.PitchVolts(0));
    CHECK(near(e.PitchVolts(1), 3.0f, 1e-4f), "B has its own");
    e.SetRef(0);
    CHECK(e.JackVolts(0) == 0.0f, "ref off: gates as played");
}

static void test_pack()
{
    Engine e = fresh(kLayoutBand);
    e.voice[2].att = 0.25f;
    e.voice[2].sus = 0.75f;
    e.voice[2].chan = 11;
    e.master.cal_off[2] = 0.02f;
    e.master.jack[7] = kFnEnvC;
    e.master.clk_ppqn = 6;
    e.master.j2_mode = kJ2StartStop;
    e.master.drum_vel = 0x05;
    e.master.drum_floor[2] = 1.2f;
    e.master.drum_trig = 0x30;
    e.master.jack[3] = kFnTrig1 + 7;
    e.master.trig_ms = 12.0f;
    e.master.cal_off[1] = -0.0345f;
    e.master.cal_scale[0] = 0.0123f;
    e.voice[1].octave = -3;
    e.voice[1].transpose = -5;
    e.voice[0].glide = 0.5f;
    e.voice[0].chan = kChanAll;
    uint8_t buf[kStateBytes];
    Pack(e.master, e.voice, 1, buf);
    MasterParams m;
    VoiceParams  v[kVoices];
    uint8_t      sel = 0;
    CHECK(Unpack(buf, m, v, sel), "unpack");
    CHECK(sel == 1 && m.jack[3] == kFnTrig1 + 7 && m.trig_ms == 12.0f, "jacks, trig");
    CHECK(near(m.cal_off[1], -0.0345f, 1e-4f) && near(m.cal_scale[0], 0.0123f, 1e-5f), "calibration");
    CHECK(v[1].octave == -3 && v[1].transpose == -5 && near(v[0].glide, 0.5f, 0.003f) && v[0].chan == kChanAll, "voices");
    CHECK(v[2].chan == 11 && near(v[2].att, 0.25f, 0.003f) && near(v[2].sus, 0.75f, 0.003f), "voice C and envelopes");
    CHECK(m.drum_vel == 0x05 && near(m.drum_floor[2], 1.2f, 0.011f) && m.drum_trig == 0x30, "drum velocity, floors, TRIG");
    CHECK(near(m.cal_off[2], 0.02f, 1e-4f) && m.jack[7] == kFnEnvC && m.clk_ppqn == 6 && m.j2_mode == kJ2StartStop,
          "C's calibration, J10, J1, J2");
    uint8_t old[kStateBytes];
    memcpy(old, buf, kStateBytes);
    old[0] = 1;
    CHECK(!Unpack(old, m, v, sel), "0.2.0's layout version: not taken");
    buf[5] ^= 1;
    CHECK(!Unpack(buf, m, v, sel), "a corrupt slot is refused");
}

static void test_packets_ring()
{
    Engine e = fresh();
    e.Packet(0x04u | 0xF0u << 8 | 0x7Du << 16 | 0x01u << 24);   /* SysEx start: ignored */
    e.Packet(0x07u | 0x90u << 8 | 0x04u << 16 | 0xF7u << 24);   /* SysEx end that looks like a note */
    CHECK(e.MidiCount() == 0, "SysEx ignored");
    uint8_t sx[16];
    int     sn;
    CHECK(!e.TakeSysex(sx, sizeof sx, &sn), "and not TETHER's");
    e.Packet(0x05u | 0xF8u << 8);                               /* a clock some devices send on CIN 5 */
    CHECK(e.ClockSeen(), "CIN 5 clock taken");
    Ring<8> r;
    for (uint32_t i = 0; i < 10; i++) r.Push(i);
    CHECK(r.dropped == 2, "full ring drops (%u)", r.dropped);
    uint32_t x, n = 0;
    while (r.Pop(&x)) CHECK(x == n++, "in order");
    CHECK(n == 8, "eight out");
}

static void test_jack_names()
{
    for (int f = 0; f < kNumJackFns; f++) CHECK(JackName((uint8_t)f)[0] != '?', "name %d", f);
}

int main()
{
    test_defaults();
    test_note_gate_pitch();
    test_legato_slide();
    test_retrig_and_priority();
    test_clock_reset_run();
    test_clock_before_transport();
    test_drums_section_accent();
    test_glide_oct_bend();
    test_cal();
    test_band();
    test_j1_clock();
    test_j2_reset();
    test_j2_start_stop();
    test_midi_wins();
    test_sysex();
    test_pack();
    test_packets_ring();
    test_jack_names();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
