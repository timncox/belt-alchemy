/* Native tests for usb_audio.h and usb_audio_desc.h (TETHER 0.5.0): the
 * descriptors the Lab serves, and its 44.1 <-> 48 kHz resamplers run against
 * a pretend FM-1 whose frame clock can be off the Lab's. */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "midi_itf_pick.h"
#include "usb_audio.h"
#include "usb_audio_desc.h"

using namespace usbaudio;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (cond) g_pass++;                                               \
        else {                                                            \
            g_fail++;                                                     \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                     \
            std::printf("\n");                                            \
        }                                                                 \
    } while (0)

static constexpr double kPi = 3.14159265358979323846;

/* ---- descriptors ------------------------------------------------------------------ */

static void test_descriptors()
{
    const uint8_t* c = kConfigDesc;
    const int      n = (int)sizeof kConfigDesc;
    std::printf("configuration: %d bytes\n", n);
    CHECK(n == 249 && (c[2] | c[3] << 8) == n, "wTotalLength %d", c[2] | c[3] << 8);
    CHECK(n < 320, "under the FM-1's 320");
    CHECK(c[4] == 4 && c[5] == 1, "four interfaces, configuration 1");
    /* walk it: every descriptor's length chains to the end */
    int p = 0, itfs = 0, alts = 0, eps = 0;
    int ac_at = -1;
    struct Ep { uint8_t itf, alt, addr, attr; uint16_t mps; uint8_t interval; } ep[8];
    uint8_t cur_itf = 0xFF, cur_alt = 0;
    bool    chained = true;
    while (p < n)
    {
        const uint8_t l = c[p], t = c[p + 1];
        if (l < 2 || p + l > n) { chained = false; break; }
        if (t == 4)
        {
            cur_itf = c[p + 2]; cur_alt = c[p + 3];
            if (cur_alt == 0) itfs++; else alts++;
        }
        if (t == 0x24 && cur_itf == 0 && c[p + 2] == 1) ac_at = p;
        if (t == 5 && eps < 8) ep[eps++] = {cur_itf, cur_alt, c[p + 2], c[p + 3], (uint16_t)(c[p + 4] | c[p + 5] << 8), c[p + 6]};
        p += l;
    }
    CHECK(chained && p == n, "descriptors chain to the end (%d of %d)", p, n);
    CHECK(itfs == 4 && alts == 2, "4 interfaces, 2 alternate settings (%d, %d)", itfs, alts);
    CHECK(ac_at > 0 && c[ac_at + 3] == 0x00 && c[ac_at + 4] == 0x01, "AC header bcdADC 1.00");
    CHECK(ac_at > 0 && (c[ac_at + 5] | c[ac_at + 6] << 8) == kAcTotal && kAcTotal == 53, "AC total %d", kAcTotal);
    CHECK(ac_at > 0 && c[ac_at + 7] == 3 && c[ac_at + 8] == 1 && c[ac_at + 9] == 2 && c[ac_at + 10] == 3,
          "bInCollection 3: 1 2 3");
    CHECK(eps == 4, "four endpoints (%d)", eps);
    CHECK(ep[0].itf == 1 && ep[0].alt == 1 && ep[0].addr == 0x02 && ep[0].attr == 0x09 && ep[0].mps == 192 && ep[0].interval == 1,
          "iso OUT 0x02 adaptive 192 on interface 1 alt 1");
    CHECK(ep[1].itf == 2 && ep[1].alt == 1 && ep[1].addr == 0x83 && ep[1].attr == 0x05 && ep[1].mps == 192 && ep[1].interval == 1,
          "iso IN 0x83 async 192 on interface 2 alt 1");
    CHECK(ep[2].itf == 3 && ep[2].addr == 0x01 && ep[2].attr == 0x02 && ep[2].mps == 64, "MIDI bulk OUT 0x01");
    CHECK(ep[3].itf == 3 && ep[3].addr == 0x81 && ep[3].attr == 0x02 && ep[3].mps == 64, "MIDI bulk IN 0x81");

    /* the format, both ways: 2 ch, 2 bytes, 16 bits, 44100 */
    int formats = 0;
    for (p = 0; p < n; p += c[p])
        if (c[p + 1] == 0x24 && c[p + 2] == 0x02 && c[p] == 11)
        {
            CHECK(c[p + 4] == 2 && c[p + 5] == 2 && c[p + 6] == 16 && c[p + 7] == 1 && (c[p + 8] | c[p + 9] << 8 | c[p + 10] << 16) == 44100,
                  "format 2 ch 16 bit 44100");
            formats++;
        }
    CHECK(formats == 2, "a format each way");

    /* the MIDI interface: libDaisy's MIDI mode, byte for byte, but for its number */
    static const uint8_t old_midi[] = {
        0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
        0x06, 0x24, 0x02, 0x01, 0x01, 0x00, 0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
        0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00, 0x09, 0x24, 0x03, 0x02, 0x06, 0x01, 0x01, 0x01, 0x00,
        0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x01,
        0x09, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x03};
    const uint8_t* m = c + n - (int)sizeof old_midi;
    CHECK(m[2] == 3 && !std::memcmp(m, old_midi, 2) && !std::memcmp(m + 3, old_midi + 3, sizeof old_midi - 3),
          "MIDI interface unchanged but for its number");
    /* and the Lab's own host-side picker (what finds the FM-1's MIDI) finds it */
    midi_pick_t pk;
    CHECK(midi_itf_pick(c, (uint16_t)n, &pk) && pk.itf[2] == 3 && pk.neps == 2, "midi_itf_pick: interface 3");

    CHECK(kDeviceDesc[0] == 18 && kDeviceDesc[4] == 0 && kDeviceDesc[17] == 1 && kDeviceDesc[7] == 64,
          "device: class 0, EP0 64, one configuration");
}

/* ---- resamplers ----------------------------------------------------------------- */

/* A pretend FM-1: its 1 ms frames by its own clock, `ppm` off the Lab's.
 * Each frame it sends a packet of 44 or 45 frames of a sine (44.1 a ms by its
 * clock) and takes an IN packet. The Lab runs 24-frame audio blocks at 48 kHz. */
struct Host
{
    double ppm = 0.0;
    double hz = 1000.0, amp = 0.5;
    bool   sending = true, taking = false;
    double phase = 0.0;      /* the sine's, in its 44.1 kHz frames */
    int    acc = 0;
    double t_next = 0.0;     /* Lab seconds of the next USB frame */
    std::vector<int> in_sizes;
    std::vector<float> in_l;

    void frame(Link& L, uint32_t now_us)
    {
        if (sending)
        {
            int n = 44;
            if (++acc >= 10) { acc = 0; n = 45; }
            uint8_t pkt[192];
            for (int i = 0; i < n; i++)
            {
                const int16_t s = ToS16((float)(amp * std::sin(2.0 * kPi * hz * phase / 44100.0)));
                const uint32_t f = Pack(s, (int16_t)-s);
                std::memcpy(pkt + 4 * i, &f, 4);
                phase += 1.0;
            }
            L.OutPacket(pkt, n * 4, now_us);
        }
        if (taking)
        {
            uint8_t pkt[192];
            const int b = L.InPacket(pkt, now_us);
            in_sizes.push_back(b / 4);
            for (int i = 0; i < b / 4; i++)
            {
                uint32_t f;
                std::memcpy(&f, pkt + 4 * i, 4);
                in_l.push_back(Left(f) / 32768.0f);
            }
        }
    }
};

struct Lab
{
    Link      L;
    OutStream out;
    InStream  ins;
    Host      host;
    double    t = 0.0;                 /* Lab seconds */
    double    in_phase = 0.0;          /* J1's sine, Lab samples */
    double    in_hz = 0.0;
    std::vector<float> ol, orr;
    int       min_fill = 1 << 30, max_fill = 0;
    float     corr_min = 1.0f, corr_max = -1.0f;
    bool      track = false;

    void run(double secs)
    {
        const int blocks = (int)std::lround(secs * kLabRate / 24.0);
        float l[24], r[24], jl[24], jr[24];
        for (int b = 0; b < blocks; b++)
        {
            const double t_end = t + 24.0 / kLabRate;
            while (host.t_next <= t)
            {
                host.frame(L, (uint32_t)std::llround(host.t_next * 1e6));
                host.t_next += 0.001 * (1.0 + host.ppm * 1e-6);   /* its ms by its clock, in Lab seconds */
            }
            for (int i = 0; i < 24; i++)
            {
                jl[i] = in_hz > 0 ? (float)(0.5 * std::sin(2.0 * kPi * in_hz * in_phase / kLabRate)) : 0.0f;
                jr[i] = 0.0f;
                in_phase += 1.0;
            }
            const uint32_t us = (uint32_t)std::llround(t * 1e6);
            ins.Process(L, jl, jr, 24, us);
            out.Process(L, l, r, 24, us);
            if (track && out.st == OutStream::kRun)
            {
                const int f = L.out.Fill();
                if (f < min_fill) min_fill = f;
                if (f > max_fill) max_fill = f;
                if (out.corr < corr_min) corr_min = out.corr;
                if (out.corr > corr_max) corr_max = out.corr;
            }
            ol.insert(ol.end(), l, l + 24);
            orr.insert(orr.end(), r, r + 24);
            t = t_end;
        }
    }
};

static float max_step(const std::vector<float>& x, size_t from = 0, size_t to = (size_t)-1)
{
    float m = 0.0f;
    if (to > x.size()) to = x.size();
    for (size_t i = from + 1; i < to; i++) m = std::fmax(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

/* the frequency, by upward zero crossings over a stretch */
static double freq(const std::vector<float>& x, size_t from, size_t to, double rate)
{
    int    first = -1, last = -1, n = 0;
    for (size_t i = from + 1; i < to && i < x.size(); i++)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            if (first < 0) first = (int)i;
            last = (int)i;
            n++;
        }
    return n > 1 ? (n - 1) * rate / (double)(last - first) : 0.0;
}

static float rms(const std::vector<float>& x, size_t from, size_t to)
{
    double a = 0;
    for (size_t i = from; i < to; i++) a += x[i] * x[i];
    return to > from ? (float)std::sqrt(a / (double)(to - from)) : 0.0f;
}

static void test_out_rate()
{
    Lab lab;
    lab.L.OpenOut(true);
    lab.run(0.2);
    CHECK(lab.out.st == OutStream::kRun && lab.out.starts == 1, "the stream starts");
    lab.track = true;
    lab.run(10.0);
    const size_t a = (size_t)(1.0 * kLabRate), b = lab.ol.size();
    const double f = freq(lab.ol, a, b, kLabRate);
    std::printf("OUT, same clocks: %.3f Hz at 48 kHz, fill %d..%d, correction %.1f ppm\n", f, lab.min_fill, lab.max_fill,
                lab.out.corr * 1e6);
    CHECK(std::fabs(f - 1000.0) < 0.1, "1 kHz in is 1 kHz out (%.3f)", f);
    CHECK(lab.out.underruns == 0 && lab.out.skips == 0, "no underruns (%u), no skips (%u)", lab.out.underruns, lab.out.skips);
    CHECK(lab.min_fill > 40 && lab.max_fill < kOutTarget + 80, "the ring stays near its target (%d..%d)", lab.min_fill, lab.max_fill);
    const float lv = rms(lab.ol, a, b), rv = rms(lab.orr, a, b);
    CHECK(std::fabs(lv - 0.3536f) < 0.005f && std::fabs(rv - 0.3536f) < 0.005f, "level kept (%.4f, %.4f)", lv, rv);
    /* L and R are the packet's two channels: R was -L */
    float worst = 0.0f;
    for (size_t i = a; i < b; i++) worst = std::fmax(worst, std::fabs(lab.ol[i] + lab.orr[i]));
    CHECK(worst < 1e-3f, "L left, R right (%.5f)", worst);
    /* against the ideal sine: the cubic's error is small at 1 kHz */
    double best = 1e9;
    for (int k = 0; k < 2000; k++)   /* find its phase */
    {
        const double ph = k * 2.0 * kPi / 2000.0;
        double e = 0;
        for (size_t i = b - 4800; i < b; i += 7)
        {
            const double want = 0.5 * std::sin(2.0 * kPi * 1000.0 * (double)(i - (b - 4800)) / kLabRate + ph);
            e = std::fmax(e, std::fabs(want - lab.ol[i]));
        }
        best = std::fmin(best, e);
    }
    std::printf("OUT: worst error against a sine %.5f\n", best);
    CHECK(best < 0.003, "close to the sine (%.5f)", best);
}

static void test_out_drift(double ppm)
{
    Lab lab;
    lab.host.ppm = ppm;
    lab.L.OpenOut(true);
    lab.run(4.0);   /* settle */
    lab.track = true;
    lab.run(20.0);
    const size_t a = lab.ol.size() - (size_t)(5 * kLabRate), b = lab.ol.size();
    const double f = freq(lab.ol, a, b, kLabRate);
    /* the FM-1's 1 kHz by its clock is 1 kHz * (1 - ppm) by the Lab's */
    const double want = 1000.0 / (1.0 + ppm * 1e-6);
    std::printf("OUT, FM-1 %+.0f ppm: %.3f Hz (want %.3f), fill %d..%d, correction %+.1f ppm\n", ppm, f, want, lab.min_fill,
                lab.max_fill, lab.out.corr * 1e6);
    CHECK(lab.out.underruns == 0 && lab.out.skips == 0, "%+.0f ppm: no underruns (%u) or skips (%u)", ppm, lab.out.underruns, lab.out.skips);
    CHECK(std::fabs(f - want) < 0.05, "%+.0f ppm: followed (%.3f, want %.3f)", ppm, f, want);
    CHECK(std::fabs(lab.out.corr * 1e6 + ppm) < 0.15 * std::fabs(ppm) + 20, "%+.0f ppm: the ratio corrected by %+.1f ppm", ppm,
          lab.out.corr * 1e6);
    CHECK(lab.min_fill > 10 && lab.max_fill < kRingFrames - 2 * kMaxFrames, "%+.0f ppm: fill bounded (%d..%d)", ppm, lab.min_fill,
          lab.max_fill);
}

/* The FM-1's frames slide past the Lab's blocks (50 ppm: a whole block every
 * 10 s): the ratio must not wander with them. */
static void test_out_steady(double ppm)
{
    Lab lab;
    lab.host.ppm = ppm;
    lab.L.OpenOut(true);
    lab.run(5.0);
    lab.track = true;
    lab.run(30.0);
    std::printf("OUT, FM-1 %+.0f ppm for 30 s: correction %+.1f..%+.1f ppm\n", ppm, lab.corr_min * 1e6, lab.corr_max * 1e6);
    CHECK((lab.corr_max - lab.corr_min) * 1e6 < 20.0, "the ratio holds steady as the clocks slide (%.1f ppm)",
          (lab.corr_max - lab.corr_min) * 1e6);
}

static void test_out_start_stop()
{
    Lab lab;
    lab.host.sending = false;
    lab.run(0.1);
    float peak = 0.0f;
    for (float x : lab.ol) peak = std::fmax(peak, std::fabs(x));
    CHECK(peak == 0.0f, "no stream: silence");
    /* a sine whose packets start mid-cycle, near its peak */
    lab.host.phase = 44100.0 / 1000.0 * 0.25;
    lab.host.sending = true;
    lab.L.OpenOut(true);
    lab.run(1.0);
    const size_t start = lab.ol.size();
    lab.host.sending = false;   /* it stops: no more packets */
    lab.L.OpenOut(false);
    lab.run(0.2);
    const float slope = (float)(0.5 * 2.0 * kPi * 1000.0 / kLabRate);   /* the sine's own biggest step */
    const float ms = max_step(lab.ol);
    std::printf("start/stop: biggest step %.4f (the sine's own %.4f)\n", ms, slope);
    CHECK(ms < slope * 1.1f, "no click starting or stopping (%.4f vs %.4f)", ms, slope);
    CHECK(lab.out.underruns == 1 && lab.out.st == OutStream::kIdle, "stopped once, idle");
    peak = 0.0f;
    for (size_t i = start + (size_t)(0.02 * kLabRate); i < lab.ol.size(); i++) peak = std::fmax(peak, std::fabs(lab.ol[i]));
    CHECK(peak == 0.0f, "20 ms after the stream stops: silence (%.6f)", peak);
    /* a gap of 10 ms mid-stream: out, then back in, still no click */
    lab.host.sending = true;
    lab.L.OpenOut(true);
    lab.run(0.5);
    lab.host.sending = false;
    lab.run(0.01);
    lab.host.sending = true;
    lab.run(0.5);
    CHECK(max_step(lab.ol) < slope * 1.1f, "a gap fades out and back in (%.4f)", max_step(lab.ol));
    CHECK(lab.out.starts == 3, "three starts (%u)", lab.out.starts);
}

static void test_out_overflow()
{
    /* the reader stalls 20 ms (it can't on the module, but): it catches up */
    Lab lab;
    lab.L.OpenOut(true);
    lab.run(0.5);
    for (int i = 0; i < 20; i++) lab.host.frame(lab.L, 0);
    float l[24], r[24];
    lab.out.Process(lab.L, l, r, 24, 0);
    CHECK(lab.out.skips == 1 && lab.L.out.Fill() <= kOutTarget, "skipped back to the target (fill %d)", lab.L.out.Fill());
}

static void test_in(double ppm)
{
    Lab lab;
    lab.host.sending = false;
    lab.host.taking  = true;
    lab.host.ppm     = ppm;
    lab.in_hz        = 1000.0;
    lab.L.OpenIn(true);
    lab.run(12.0);
    const std::vector<int>& s = lab.host.in_sizes;
    bool sizes = true;
    for (int x : s) sizes = sizes && (x == 44 || x == 45);
    CHECK(sizes, "%+.0f ppm: every packet 44 or 45 frames", ppm);
    /* the first packets are silence until the ring fills, then it fades in */
    CHECK(lab.host.in_l.size() > 100 && lab.host.in_l[0] == 0.0f, "silence first");
    CHECK(max_step(lab.host.in_l) < (float)(0.5 * 2.0 * kPi * 1000.0 / 44100.0) * 1.1f, "%+.0f ppm: no click (%.4f)", ppm,
          max_step(lab.host.in_l));
    /* the average: 44.1 a ms by the Lab's clock, so 44.1 * (1 + ppm) a host ms */
    long tot = 0;
    const size_t from = s.size() / 3;
    for (size_t i = from; i < s.size(); i++) tot += s[i];
    const double avg = (double)tot / (double)(s.size() - from), want = 44.1 * (1.0 + ppm * 1e-6);
    std::printf("IN, FM-1 %+.0f ppm: %.5f frames a packet (want %.5f), %u more, %u fewer, %u underruns, fill %d\n", ppm, avg,
                want, lab.L.sz_more, lab.L.sz_fewer, lab.L.sz_underruns, lab.L.in.Fill());
    CHECK(std::fabs(avg - want) < 0.002, "%+.0f ppm: average %.5f (want %.5f)", ppm, avg, want);
    CHECK(lab.L.sz_underruns == 0 && lab.L.in.dropped == 0, "%+.0f ppm: no underruns (%u), none dropped (%u)", ppm,
          lab.L.sz_underruns, lab.L.in.dropped);
    CHECK(std::abs(lab.L.in.Fill() - kInTarget) < 40, "%+.0f ppm: fill near its target (%d)", ppm, lab.L.in.Fill());
    /* 1 kHz on J1 is 1 kHz at 44.1 kHz (by the Lab's clock) */
    const std::vector<float>& x = lab.host.in_l;
    const double f = freq(x, x.size() / 2, x.size(), 44100.0);
    CHECK(std::fabs(f - 1000.0) < 1.0, "%+.0f ppm: 1 kHz back (%.3f)", ppm, f);
    /* closed: nothing more goes in */
    lab.L.OpenIn(false);
    lab.run(0.1);
    CHECK(lab.L.in.Fill() <= kInTarget + 50, "closed: the ring stops filling");
}

static void test_ring()
{
    Ring<8> r;
    for (uint32_t i = 0; i < 10; i++) r.Push(i);
    CHECK(r.Fill() == 8 && r.dropped == 2, "full at 8, two dropped");
    r.Skip(3);
    uint32_t x;
    CHECK(r.Pop(&x) && x == 3 && r.Fill() == 4, "skip");
    r.Drain();
    CHECK(r.Fill() == 0 && !r.Pop(&x), "drained");
    CHECK(Left(Pack(-2, 3)) == -2 && Right(Pack(-2, 3)) == 3 && ToS16(1.5f) == 32767 && ToS16(-1.5f) == -32768, "packing");
}

int main()
{
    test_descriptors();
    test_ring();
    test_out_rate();
    /* the FM-1 sends at its I2S rate, 44117.6 Hz: +399.1 ppm on 44.1k */
    test_out_drift((44117.647 / 44100.0 - 1.0) * 1e6);
    test_out_drift(-400.0);
    test_out_drift(+500.0);
    test_out_drift(-500.0);
    test_out_drift(+2000.0);
    test_out_steady(50.0);
    test_out_steady((44117.647 / 44100.0 - 1.0) * 1e6);   /* the FM-1 as it is */
    test_out_start_stop();
    test_out_overflow();
    test_in(0.0);
    test_in(+400.0);
    test_in(-400.0);
    test_in(+500.0);
    test_in(-500.0);
    std::printf("usb_audio: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
