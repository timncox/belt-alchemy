/*
 * Native tests for tether_engine -- no hardware. Runs the engine as the
 * firmware does (24-frame blocks at 48 kHz) and feeds it the USB-MIDI
 * packets PROTECTOR's PARTS setting sends.
 */
#include <cmath>
#include <cstdio>
#include <cstring>

#include "tether_engine.h"

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

static Engine fresh()
{
    Engine e;
    e.Init(kSr);
    for (int j = 0; j < kJacks; j++) g_prev[j] = false;
    return e;
}

static bool near(float a, float b, float tol = 0.002f) { return std::fabs(a - b) < tol; }

static void test_defaults()
{
    Engine e = fresh();
    CHECK(e.voice[0].chan == 4 && e.voice[1].chan == 5, "A on ch5 BASS, B on ch6 LEAD");
    CHECK(e.master.jack[0] == kFnGateA && e.master.jack[1] == kFnGateB, "J3 J4 gates");
    CHECK(e.master.jack[2] == kFnClk16 && e.master.jack[3] == kFnReset, "J5 clock J6 reset");
    CHECK(e.master.jack[4] == kFnTrig1 && e.master.jack[5] == kFnTrig1 + 1, "J7 KICK J8 SNARE");
    for (int j = 0; j < kJacks; j++) CHECK(e.JackVolts(j) == 0.0f, "jack %d quiet at boot", j);
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
    on(e, 1, 36, 100);
    CHECK(e.JackVolts(4) == kGateV && e.JackVolts(5) == 0.0f, "KICK trig on J7");
    Run r = run(e, 10);
    CHECK(e.JackVolts(4) == 0.0f && r.high_blocks[4] >= 9 && r.high_blocks[4] <= 11, "5 ms (%d blocks)", r.high_blocks[4]);
    on(e, 2, 38, 100);
    CHECK(e.JackVolts(5) == kGateV, "SNARE trig on J8");

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
    CHECK(e.JackVolts(0) == kAccentV, "ACC GATE: accented = 8 V");
    off(e, 5, 48);
    on(e, 5, 48, 90);
    run(e, 4);
    CHECK(e.JackVolts(0) == kGateV, "ACC GATE: plain = 5 V");
    e.voice[0].gate_mode = kVelGate;
    CHECK(near(e.JackVolts(0), 1.0f + 7.0f * 90.0f / 127.0f), "VEL GATE height");
    e.master.jack[1] = kFnVelA;
    CHECK(near(e.JackVolts(1), 8.0f * 90.0f / 127.0f), "VEL A");
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
    Engine e = fresh();
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
    buf[5] ^= 1;
    CHECK(!Unpack(buf, m, v, sel), "a corrupt slot is refused");
}

static void test_packets_ring()
{
    Engine e = fresh();
    e.Packet(0x04u | 0xF0u << 8 | 0x7Du << 16 | 0x01u << 24);   /* SysEx start: ignored */
    e.Packet(0x07u | 0x90u << 8 | 0x04u << 16 | 0xF7u << 24);   /* SysEx end that looks like a note */
    CHECK(e.MidiCount() == 0, "SysEx ignored");
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
    test_pack();
    test_packets_ring();
    test_jack_names();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
