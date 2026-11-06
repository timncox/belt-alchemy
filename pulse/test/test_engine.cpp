/*
 * Native tests for pulse_engine — no hardware. Drives the engine exactly as
 * the firmware does: 24-frame blocks at 48 kHz, J1/J2 as codec-scale
 * sample buffers.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "pulse_engine.h"

using namespace pulse;

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) { g_pass++; }                                       \
        else {                                                        \
            g_fail++;                                                 \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

static constexpr float  kSr    = 48000.0f;
static constexpr size_t kBlock = 24;

/* Signal generators return panel volts at time t. */
using Sig = std::function<float(double)>;

struct Run
{
    std::vector<double> rises[kNumOuts];   /* times of output rising edges */
    std::vector<double> falls[kNumOuts];
    double high_time[kNumOuts] = {};
    double total = 0.0;
    float  vmin[kNumOuts], vmax[kNumOuts];
};

static Run run(Engine& e, double seconds, Sig clk = nullptr, Sig rst = nullptr,
               float thresh = 2.5f, std::function<void(double)> each = nullptr)
{
    Run r;
    for (int c = 0; c < kNumOuts; c++) { r.vmin[c] = 1e9f; r.vmax[c] = -1e9f; }
    bool hi[kNumOuts];
    for (int c = 0; c < kNumOuts; c++) hi[c] = e.Volts(c) > thresh;
    float cb[kBlock], rb[kBlock];
    const size_t blocks = (size_t)(seconds * kSr / kBlock);
    for (size_t b = 0; b < blocks; b++)
    {
        const double t0 = e.Now();
        for (size_t i = 0; i < kBlock; i++)
        {
            const double t = t0 + (double)(i + 1) / kSr;
            cb[i] = clk ? clk(t) / 5.0f : 0.0f;
            rb[i] = rst ? rst(t) / 5.0f : 0.0f;
        }
        e.Process(clk ? cb : nullptr, rst ? rb : nullptr, kBlock);
        if (each) each(e.Now());
        const double t = e.Now();
        for (int c = 0; c < kNumOuts; c++)
        {
            const float v = e.Volts(c);
            if (v < r.vmin[c]) r.vmin[c] = v;
            if (v > r.vmax[c]) r.vmax[c] = v;
            const bool h = v > thresh;
            if (h && !hi[c]) r.rises[c].push_back(t);
            if (!h && hi[c]) r.falls[c].push_back(t);
            hi[c] = h;
            if (h) r.high_time[c] += kBlock / kSr;
        }
        r.total += kBlock / kSr;
    }
    return r;
}

static void fresh(Engine& e)
{
    e.Init(kSr);
    for (int c = 0; c < kNumOuts; c++) e.out[c] = OutParams{};
    e.master = MasterParams{};
}

/* A clock of `ppqn` pulses per beat at `bpm`, 5 V, 2 ms wide. */
static Sig clock_sig(float bpm, int ppqn, double start = 0.0)
{
    const double period = 60.0 / bpm / ppqn;
    return [=](double t) -> float {
        if (t < start) return 0.0f;
        const double ph = std::fmod(t - start, period);
        return ph < 0.002 ? 5.0f : 0.0f;
    };
}

/* ---------------------------------------------------------------------------- */

static void test_ratios_internal()
{
    Engine e;
    fresh(e);
    e.master.bpm   = 120.0f;
    e.out[0].ratio = kRatioUnity;  /* x1: 2 Hz  */
    e.out[1].ratio = 9;            /* /4: 0.5 Hz */
    e.out[2].ratio = 16;           /* x3: 6 Hz  */
    e.out[3].ratio = 12;           /* /1.5: 4/3 Hz */
    e.out[4].ratio = 25;           /* x32: 64 Hz */
    Run r = run(e, 9.99);   /* stop short of the edge at exactly 10 s */
    CHECK(r.rises[0].size() == 20, "x1 rises %zu", r.rises[0].size());
    CHECK(r.rises[1].size() == 5, "/4 rises %zu", r.rises[1].size());
    CHECK(r.rises[2].size() == 60, "x3 rises %zu", r.rises[2].size());
    CHECK(r.rises[3].size() == 14, "/1.5 rises %zu", r.rises[3].size());
    CHECK(r.rises[4].size() >= 639 && r.rises[4].size() <= 641, "x32 rises %zu", r.rises[4].size());
    const double duty = r.high_time[0] / r.total;
    CHECK(std::fabs(duty - 0.5) < 0.01, "x1 duty %.3f", duty);
    /* x1 rising edges land on the beat grid within one block. */
    for (size_t i = 0; i < r.rises[0].size(); i++)
    {
        const double want = 0.5 * (double)i;
        CHECK(std::fabs(r.rises[0][i] - want) <= kBlock / kSr + 1e-9,
              "edge %zu at %.5f want %.5f", i, r.rises[0][i], want);
    }
    /* x3 and x1 share every third edge. */
    CHECK(std::fabs(r.rises[2][3] - r.rises[0][1]) < 1e-9, "x3/x1 alignment");
}

static void test_width_and_levels()
{
    Engine e;
    fresh(e);
    e.out[0].width = 0.25f;
    e.out[1].wave  = kSaw;
    e.out[2].wave  = kSine;
    e.out[2].offset = -2.5f;
    e.out[3].wave   = kTrig;
    e.out[3].width  = 0.0f;              /* 5 ms */
    e.out[4].wave   = kGate;
    e.out[4].level  = 0.5f;              /* 2.5 V gates */
    Run r = run(e, 4.0, nullptr, nullptr, 1.0f);
    const double duty = r.high_time[0] / r.total;
    CHECK(std::fabs(duty - 0.25) < 0.01, "width 25%% duty %.3f", duty);
    CHECK(r.vmin[1] >= -0.001f && r.vmax[1] > 4.95f && r.vmax[1] <= 5.0f,
          "saw range %.3f..%.3f", r.vmin[1], r.vmax[1]);
    CHECK(r.vmin[2] < -2.45f && r.vmax[2] > 2.45f && r.vmax[2] <= 2.5f,
          "bipolar sine %.3f..%.3f", r.vmin[2], r.vmax[2]);
    const double trig_duty = r.high_time[3] / r.total;   /* 5 ms of 500 */
    CHECK(trig_duty > 0.008 && trig_duty < 0.012, "trig duty %.4f", trig_duty);
    CHECK(std::fabs(r.vmax[4] - 2.5f) < 0.001f, "level 0.5 -> %.3f V", r.vmax[4]);
}

static void test_euclid()
{
    /* Bresenham pattern, 3 in 8: x..x..x. */
    const char* want = "x..x..x.";
    char got[9] = {};
    for (int i = 0; i < 8; i++) got[i] = EuclidHit(i, 8, 3, 0) ? 'x' : '.';
    int hits = 0;
    for (int i = 0; i < 8; i++) hits += got[i] == 'x';
    CHECK(hits == 3, "E(3,8) hits %d (%s)", hits, got);
    /* Evenly spread: no gap longer than ceil(8/3)=3 steps. */
    (void)want;
    int maxgap = 0, last = -1, first = -1;
    for (int i = 0; i < 8; i++)
        if (got[i] == 'x') { if (last >= 0 && i - last > maxgap) maxgap = i - last; if (first < 0) first = i; last = i; }
    if (8 - last + first > maxgap) maxgap = 8 - last + first;
    CHECK(maxgap <= 3, "E(3,8) max gap %d (%s)", maxgap, got);
    /* Rotation shifts the pattern. */
    for (int i = 0; i < 8; i++)
        CHECK(EuclidHit(i, 8, 3, 1) == EuclidHit(i + 1, 8, 3, 0), "rotate step %d", i);
    CHECK(EuclidHit(5, 16, 16, 0) && !EuclidHit(5, 16, 0, 0), "full / empty");

    Engine e;
    fresh(e);
    e.out[0].ratio = 15;   /* x2: 4 Hz at 120 */
    e.out[0].steps = 8;
    e.out[0].fills = 3;
    Run r = run(e, 3.99);  /* 16 eighths = 2 patterns */
    CHECK(r.rises[0].size() == 6, "euclid rises %zu", r.rises[0].size());
}

static void test_swing()
{
    Engine e;
    fresh(e);
    e.out[0].ratio = 15;      /* eighths, 0.25 s apart straight */
    e.out[0].swing = 2.0f / 3.0f;
    e.out[0].width = 0.1f;
    Run r = run(e, 2.0);
    CHECK(r.rises[0].size() == 8, "swing rises %zu", r.rises[0].size());
    if (r.rises[0].size() >= 4)
    {
        const double a = r.rises[0][1] - r.rises[0][0];
        const double b = r.rises[0][2] - r.rises[0][1];
        /* Triplet swing: long-short = 2/3 and 1/3 of a beat. */
        CHECK(std::fabs(a - 1.0 / 3.0) < 0.002, "swung long %.4f", a);
        CHECK(std::fabs(b - 1.0 / 6.0) < 0.002, "swung short %.4f", b);
    }
    CHECK(std::fabs(SwingWarp(3.7, 0.5f) - 3.7) < 1e-12, "swing 50%% is identity");
}

static void test_phase()
{
    Engine e;
    fresh(e);
    e.out[1].phase = 0.75f;   /* a quarter cycle early */
    Run r = run(e, 2.0);
    CHECK(r.rises[0].size() == 4 && r.rises[1].size() == 4, "phase rises %zu %zu",
          r.rises[0].size(), r.rises[1].size());
    /* Phase 0.75 starts three quarters into the cycle: next edge after
     * a quarter cycle (0.125 s at 120). */
    if (r.rises[1].size() >= 2)
        CHECK(std::fabs(r.rises[1][0] - 0.125) <= kBlock / kSr + 1e-9,
              "phase 0.75 edge at %.4f", r.rises[1][0]);
}

static void test_chance()
{
    Engine e;
    fresh(e);
    e.out[0].ratio  = 21;   /* x8 */
    e.out[0].chance = 0.0f;
    e.out[1].ratio  = 21;
    e.out[1].chance = 0.5f;
    e.out[1].width  = 0.5f;
    Run r1 = run(e, 10.0);   /* 160 cycles */
    CHECK(r1.rises[0].empty(), "chance 0 fired %zu", r1.rises[0].size());
    const size_t n = r1.rises[1].size();
    CHECK(n > 50 && n < 110, "chance 0.5 fired %zu of 160 (merged runs count once)", n);

    /* Same reset, same sequence. */
    Engine a, b;
    fresh(a); fresh(b);
    a.out[0].ratio = b.out[0].ratio = 21;
    a.out[0].chance = b.out[0].chance = 0.4f;
    a.out[0].width = b.out[0].width = 0.3f;
    Run ra = run(a, 5.0), rb = run(b, 5.0);
    CHECK(ra.rises[0] == rb.rises[0], "chance repeatable");
}

static void test_tap()
{
    Engine e;
    fresh(e);
    e.Tap(10.0);
    e.Tap(10.5);
    CHECK(std::fabs(e.Bpm() - 120.0f) < 0.01f, "tap 2 -> %.2f", e.Bpm());
    e.Tap(11.0);
    e.Tap(11.5);
    e.Tap(12.0);
    CHECK(std::fabs(e.Bpm() - 120.0f) < 0.01f, "tap 5 -> %.2f", e.Bpm());
    e.Tap(20.0);   /* > 2 s gap starts over */
    CHECK(std::fabs(e.Bpm() - 120.0f) < 0.01f, "lone tap keeps tempo %.2f", e.Bpm());
    e.Tap(20.4);
    CHECK(std::fabs(e.Bpm() - 150.0f) < 0.01f, "tap -> %.2f", e.Bpm());
}

static void test_external_clock()
{
    Engine e;
    fresh(e);
    e.master.bpm      = 120.0f;   /* internal, to be overridden */
    e.master.ppqn_idx = 5;        /* 24 PPQN */
    e.out[0].ratio    = kRatioUnity;
    e.out[1].ratio    = 17;       /* x4 */
    e.out[2].ratio    = 9;        /* /4 */
    const double start = 0.1;
    Run r = run(e, 6.1, clock_sig(100.0f, 24, start));
    CHECK(e.ExtLocked(), "locked");
    CHECK(std::fabs(e.Bpm() - 100.0f) < 0.2f, "ext bpm %.3f", e.Bpm());
    /* 100 BPM for 6 s = 10 beats: x1 = 10 or 11 edges (the first pulse is
     * a downbeat). */
    CHECK(r.rises[0].size() == 10 || r.rises[0].size() == 11, "ext x1 rises %zu", r.rises[0].size());
    /* After the first beat, x1 edges sit on every 24th pulse. */
    bool aligned = true;
    for (size_t i = 1; i < r.rises[0].size(); i++)
    {
        const double want = start + 0.6 * (double)i;
        if (std::fabs(r.rises[0][i] - want) > 2.0 * kBlock / kSr) aligned = false;
    }
    CHECK(aligned, "x1 aligned to the clock");
    CHECK(r.rises[1].size() >= 39 && r.rises[1].size() <= 41, "ext x4 rises %zu", r.rises[1].size());
    CHECK(r.rises[2].size() == 3, "ext /4 rises %zu", r.rises[2].size());

    /* Clock stops: within the timeout the engine falls back to 120. */
    run(e, 1.0);
    CHECK(!e.ExtLocked(), "unlocked after silence");
    CHECK(std::fabs(e.Bpm() - 120.0f) < 0.01f, "internal again %.2f", e.Bpm());
}

static void test_ext_tempo_change()
{
    /* x8 multiplication should follow a tempo change within a beat or two
     * and never run ahead of the clock. */
    Engine e;
    fresh(e);
    e.master.ppqn_idx = 0;      /* 1 PPQN */
    e.out[0].ratio = 21;        /* x8 */
    auto clk = [](double t) -> float {
        /* 120 BPM for 4 s, then 90 BPM. */
        double ph;
        if (t < 4.0) ph = std::fmod(t, 0.5);
        else         ph = std::fmod(t - 4.0, 60.0 / 90.0);
        return ph < 0.002 ? 5.0f : 0.0f;
    };
    Run r = run(e, 10.0, clk);
    /* 8 beats at 120 (64 edges) + 9 beats at 90 (72) ≈ 136. */
    const size_t n = r.rises[0].size();
    CHECK(n >= 128 && n <= 138, "x8 through tempo change %zu", n);
    CHECK(std::fabs(e.Bpm() - 90.0f) < 0.5f, "settled at %.2f", e.Bpm());
}

static void test_reset()
{
    Engine e;
    fresh(e);
    e.out[0].ratio = 9;   /* /4: one edge per 2 s */
    /* J2 reset pulse at 1.0 s: the bar restarts there. */
    auto rst = [](double t) -> float { return (t >= 1.0 && t < 1.002) ? 5.0f : 0.0f; };
    Run r = run(e, 3.5, nullptr, rst);
    /* 0 s, the reset at 1 s, then a bar later at 3 s (not 2 s). */
    CHECK(r.rises[0].size() == 3, "rises %zu", r.rises[0].size());
    if (r.rises[0].size() == 3)
    {
        CHECK(r.rises[0][0] < 0.001, "first at %.4f", r.rises[0][0]);
        CHECK(std::fabs(r.rises[0][1] - 1.0) < 0.001, "reset edge at %.4f", r.rises[0][1]);
        CHECK(std::fabs(r.rises[0][2] - 3.0) < 0.001, "next bar at %.4f", r.rises[0][2]);
    }
    /* Start/stop mode: a J2 edge stops, the next starts from the top. */
    Engine s;
    fresh(s);
    s.master.j2_mode = kJ2StartStop;
    auto ss = [](double t) -> float {
        return ((t >= 1.0 && t < 1.002) || (t >= 2.0 && t < 2.002)) ? 5.0f : 0.0f;
    };
    run(s, 1.5, nullptr, ss);
    CHECK(!s.Running(), "stopped by J2");
    const double pos_stopped = s.BeatPos();
    Run r2 = run(s, 0.4, nullptr, [&](double t) { return ss(t + 1.5); });
    CHECK(std::fabs(s.BeatPos() - pos_stopped) < 1e-9, "position held while stopped");
    (void)r2;
}

static void test_random_and_slew()
{
    Engine e;
    fresh(e);
    e.out[0].wave = kStepRand;
    e.out[1].wave = kStepRand;
    e.out[1].slew = 0.5f;
    std::vector<float> a, b;
    run(e, 3.0, nullptr, nullptr, 2.5f, [&](double) {
        a.push_back(e.Volts(0));
        b.push_back(e.Volts(1));
    });
    int changes = 0;
    for (size_t i = 1; i < a.size(); i++) changes += a[i] != a[i - 1];
    CHECK(changes >= 5 && changes <= 6, "step random changes %d", changes);
    float maxjump_a = 0, maxjump_b = 0;
    for (size_t i = 1; i < a.size(); i++)
    {
        maxjump_a = std::fmax(maxjump_a, std::fabs(a[i] - a[i - 1]));
        maxjump_b = std::fmax(maxjump_b, std::fabs(b[i] - b[i - 1]));
    }
    CHECK(maxjump_b < 0.25f * maxjump_a + 1e-6f, "slew smooths: %.3f vs %.3f", maxjump_b, maxjump_a);
}

static void test_persistence()
{
    MasterParams m, m2;
    OutParams    o[kNumOuts], o2[kNumOuts];
    Defaults(m, o);
    m.bpm = 97.5f;
    m.j2_mode = kJ2StartStop;
    o[3].offset = -1.25f;
    o[3].swing  = 0.6f;
    o[3].steps  = 13; o[3].fills = 5; o[3].rotate = 4;
    uint8_t buf[kStateBytes];
    Pack(m, o, 6, buf);
    uint8_t sel = 0;
    CHECK(Unpack(buf, m2, o2, sel), "unpack");
    CHECK(sel == 6 && m2.bpm == 97.5f && m2.j2_mode == kJ2StartStop, "master");
    bool same = true;
    for (int c = 0; c < kNumOuts; c++)
    {
        const OutParams& x = o[c];
        const OutParams& y = o2[c];
        if (x.ratio != y.ratio || x.wave != y.wave || x.steps != y.steps ||
            x.fills != y.fills || x.rotate != y.rotate) same = false;
        if (std::fabs(x.width - y.width) > 1e-4f || std::fabs(x.level - y.level) > 1e-4f ||
            std::fabs(x.offset - y.offset) > 1e-3f || std::fabs(x.phase - y.phase) > 1e-4f ||
            std::fabs(x.chance - y.chance) > 1e-4f || std::fabs(x.swing - y.swing) > 1e-4f ||
            std::fabs(x.slew - y.slew) > 1e-4f) same = false;
    }
    CHECK(same, "outputs round-trip");
    buf[7] = 99;
    CHECK(!Unpack(buf, m2, o2, sel), "foreign layout refused");
}

static void test_stop_holds_gates_low()
{
    Engine e;
    fresh(e);
    e.out[0].width = 1.0f;   /* 100% width: tie */
    run(e, 0.2);
    CHECK(e.Volts(0) > 4.9f, "tie high %.2f", e.Volts(0));
    e.Stop();
    run(e, 0.05);
    CHECK(e.Volts(0) < 0.01f, "gate low when stopped %.2f", e.Volts(0));
    e.Start();
    e.Reset();
    run(e, 0.01);
    CHECK(e.Volts(0) > 4.9f, "restarted");
}

int main()
{
    test_ratios_internal();
    test_width_and_levels();
    test_euclid();
    test_swing();
    test_phase();
    test_chance();
    test_tap();
    test_external_clock();
    test_ext_tempo_change();
    test_reset();
    test_random_and_slew();
    test_persistence();
    test_stop_holds_gates_low();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
