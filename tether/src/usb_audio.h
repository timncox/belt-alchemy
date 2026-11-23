/*
 * usb_audio -- TETHER 0.5.0's USB audio, the part with no SDK in it
 * (natively tested in ../test/test_usb_audio.cpp).
 *
 * The FM-1 is the USB host on the cable (PROTECTOR's CLOCK OUT on LAB), the
 * Lab its device: besides PARTS MIDI, the FM-1 sends its sound and takes the
 * Lab's J1 / J2 back, both as 16-bit stereo at 44.1 kHz, one isochronous
 * packet a millisecond of 44 or 45 frames (the contract:
 * usb-audio-contract.md, shared with the FM-1 side). The Lab's codec runs at
 * 48 kHz, so both ways go through a resampler:
 *
 *   OUT, FM-1 -> J9 / J10 (adaptive). The port drops each packet into a
 *   ring (Link::OutPacket); the audio callback reads it at 48 kHz through
 *   OutStream, which steers its 44.1 -> 48 ratio by how full the ring is,
 *   so the FM-1's clock is followed. It starts once the ring holds kOutTarget
 *   frames, fades in and out over 5 ms, and is silent when nothing comes.
 *
 *   IN, J1 / J2 -> FM-1 (asynchronous). The audio callback turns each block
 *   into 44.1 kHz frames at the fixed ratio by the Lab's own clock (InStream)
 *   into a ring; the port takes 44 or 45 of them each frame (InSizer): nine
 *   44s to a 45, plus one more or one fewer when the ring drifts from
 *   kInTarget, the FM-1's frame clock being not quite the Lab's.
 *
 * A frame is one uint32: left in the low 16 bits, right in the high, which is
 * the packet's own byte order (little endian), so a packet copies straight in.
 *
 * How full a ring is depends on when it's looked at: a packet adds 44 frames
 * at once each millisecond, a 0.5 ms block takes 22, and the two clocks slide
 * past each other, so a look at the start of each block would see the fill
 * swing by 22 frames as they do, and the OUT ratio would wander with it
 * (hundreds of ppm, for seconds). So each side stamps what it last put in
 * with the time (Mark), and the reader counts that last lump as if it came in
 * evenly over its period (SmoothFill): what is left is the true drift.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#include <atomic>

namespace usbaudio
{

constexpr int   kUsbRate        = 44100;
constexpr int   kLabRate        = 48000;
constexpr int   kMaxPacketBytes = 192;                   /* wMaxPacketSize */
constexpr int   kMaxFrames      = kMaxPacketBytes / 4;   /* 48 */
constexpr int   kRingFrames     = 512;                   /* 11.6 ms at 44.1 kHz */
constexpr int   kOutTarget      = 132;                   /* 3 ms: where the OUT ring is held */
constexpr int   kInTarget       = 96;                    /* 2.2 ms: where the IN ring is held */
constexpr float kOutNominal     = (float)kUsbRate / (float)kLabRate;   /* 44.1 frames in per 48 out */
constexpr float kInStep         = (float)kLabRate / (float)kUsbRate;   /* 48 frames in per 44.1 out */
constexpr int   kFadeSamples    = 240;                   /* 5 ms at 48 kHz */
constexpr float kOutKp          = 4e-5f;                 /* ratio per frame of fill error: ~0.6 s to settle */
constexpr float kOutMaxCorr     = 0.01f;                 /* the FM-1's clock may be 1 % off, no more */
constexpr float kOutFillTau     = 0.05f;                 /* s: the fill measure's smoothing */
constexpr int   kSizerHyst      = 4;                     /* frames either side of kInTarget */
constexpr uint32_t kPacketUs    = 1000;                  /* a USB frame */
constexpr uint32_t kBlockUs     = 500;                   /* the Lab's audio block (24 frames) */

inline uint32_t Pack(int16_t l, int16_t r) { return (uint32_t)(uint16_t)l | (uint32_t)(uint16_t)r << 16; }
inline int16_t  Left(uint32_t f) { return (int16_t)(uint16_t)(f & 0xFFFFu); }
inline int16_t  Right(uint32_t f) { return (int16_t)(uint16_t)(f >> 16); }
inline int16_t  ToS16(float x)
{
    const float y = x * 32767.0f;
    return (int16_t)(y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : (int)(y + (y >= 0.0f ? 0.5f : -0.5f)));
}

/* Single producer, single consumer. Fill() is right on either side (it may
 * be a frame stale on the other). */
template <int N>
struct Ring
{
    std::atomic<uint32_t> w{0}, r{0};
    uint32_t              buf[N];
    uint32_t              dropped = 0;   /* producer's count */

    int  Fill() const { return (int)(w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire)); }
    bool Push(uint32_t f)
    {
        const uint32_t wi = w.load(std::memory_order_relaxed);
        if ((int)(wi - r.load(std::memory_order_acquire)) >= N) { dropped++; return false; }
        buf[wi % (uint32_t)N] = f;
        w.store(wi + 1u, std::memory_order_release);
        return true;
    }
    bool Pop(uint32_t* f)
    {
        const uint32_t ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        *f = buf[ri % (uint32_t)N];
        r.store(ri + 1u, std::memory_order_release);
        return true;
    }
    /* consumer only */
    void Skip(int n)
    {
        const int k = n < Fill() ? n : Fill();
        if (k > 0) r.store(r.load(std::memory_order_relaxed) + (uint32_t)k, std::memory_order_release);
    }
    void Drain() { r.store(w.load(std::memory_order_acquire), std::memory_order_release); }
};

/* Catmull-Rom between y1 and y2, t 0..1. */
inline float Cubic(float y0, float y1, float y2, float y3, float t)
{
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

/* The writer's last lump: when (us) and how many frames. A sequence count
 * around it, odd while the writer is between its pushes and its stamp. */
struct Mark
{
    std::atomic<uint32_t> seq{0}, t_us{0}, frames{0};
    void Begin() { seq.fetch_add(1u, std::memory_order_acq_rel); }
    void End(uint32_t now_us, uint32_t n)
    {
        t_us.store(now_us, std::memory_order_relaxed);
        frames.store(n, std::memory_order_relaxed);
        seq.fetch_add(1u, std::memory_order_release);
    }
};

/* The ring's fill at now_us with the writer's last lump spread over its
 * period. Returns false (nothing changed) when the writer is mid-lump. */
template <class R>
inline bool SmoothFill(const R& ring, const Mark& m, uint32_t now_us, uint32_t period_us, float* fill)
{
    for (int k = 0; k < 2; k++)
    {
        const uint32_t s = m.seq.load(std::memory_order_acquire);
        if (s & 1u) return false;
        const int      f  = ring.Fill();
        const uint32_t t  = m.t_us.load(std::memory_order_relaxed);
        const uint32_t n  = m.frames.load(std::memory_order_relaxed);
        if (m.seq.load(std::memory_order_acquire) != s) continue;
        const uint32_t dt = now_us - t;
        *fill = dt >= period_us ? (float)f : (float)f - (float)n * (1.0f - (float)dt / (float)period_us);
        return true;
    }
    return false;
}

/* Both streams, shared by the port (USB interrupt) and the audio callback. */
struct Link
{
    Ring<kRingFrames> out;   /* FM-1 -> Lab: the port writes, the audio callback reads */
    Ring<kRingFrames> in;    /* Lab -> FM-1: the audio callback writes, the port reads */
    Mark              out_mark, in_mark;
    std::atomic<bool> out_on{false}, in_on{false};
    std::atomic<uint32_t> out_packets{0}, in_packets{0}, odd_packets{0};

    /* the port: the host set alt 1 (or 0) on the OUT / IN interface */
    void OpenOut(bool on) { out_on.store(on, std::memory_order_release); }
    void OpenIn(bool on);
    /* the port: an OUT packet arrived (bytes as received) */
    void OutPacket(const uint8_t* p, int bytes, uint32_t now_us)
    {
        if (bytes & 3) odd_packets.fetch_add(1, std::memory_order_relaxed);
        int n = 0;
        out_mark.Begin();
        for (int i = 0; i + 4 <= bytes && i < kMaxPacketBytes; i += 4, n++)
        {
            uint32_t f;
            memcpy(&f, p + i, 4);
            out.Push(f);
        }
        out_mark.End(now_us, (uint32_t)n);
        out_packets.fetch_add(1, std::memory_order_relaxed);
    }
    /* the port: the next IN packet into p (kMaxPacketBytes of room). Returns bytes. */
    int InPacket(uint8_t* p, uint32_t now_us);

    /* InSizer's state (the port's) */
    int      sz_acc = 0;
    bool     sz_primed = false;
    float    sz_fill = (float)kInTarget;
    uint32_t sz_underruns = 0, sz_more = 0, sz_fewer = 0;
};

/* ---- OUT: the ring at 44.1 kHz to the codec at 48 kHz (adaptive) ---------------- */

struct OutStream
{
    enum State : uint8_t { kIdle, kRun, kFade };
    State    st   = kIdle;
    float    h[4][2] = {};       /* the four frames around the read point, -1..1 */
    float    frac = 0.0f;        /* between h[1] and h[2] */
    float    gain = 0.0f;
    float    fill_f = 0.0f;      /* the ring's fill, smoothed */
    float    corr = 0.0f;        /* the ratio's correction: + reads faster */
    uint32_t starts = 0, underruns = 0, skips = 0;

    float Step() const { return kOutNominal * (1.0f + corr); }

    /* n frames at 48 kHz into l, r (-1..1, 1 = 5 V at the jack), at now_us
     * (the block's time, any steady microsecond clock). While the host has
     * the stream closed (alt 0), whatever is left in the ring once it has
     * played out is thrown away, so the next stream starts clean. */
    void Process(Link& L, float* l, float* r, int n, uint32_t now_us)
    {
        Ring<kRingFrames>& ring = L.out;
        const bool on = L.out_on.load(std::memory_order_acquire);
        if (st == kIdle)
        {
            if (on && ring.Fill() >= kOutTarget)
            {
                /* start: the history is the first frame, the gain comes up */
                uint32_t f = 0;
                ring.Pop(&f);
                for (int k = 0; k < 4; k++) { h[k][0] = Left(f) / 32768.0f; h[k][1] = Right(f) / 32768.0f; }
                frac   = 0.0f;
                fill_f = (float)kOutTarget;
                corr   = 0.0f;
                st     = kRun;
                starts++;
            }
            else
            {
                if (!on) ring.Drain();
                for (int i = 0; i < n; i++) l[i] = r[i] = 0.0f;
                return;
            }
        }
        if (st == kRun)
        {
            const int now = ring.Fill();
            if (now > kRingFrames - 2 * kMaxFrames)   /* the reader fell far behind: catch up */
            {
                ring.Skip(now - kOutTarget);
                skips++;
                fill_f = (float)kOutTarget;
            }
            float fill;
            if (SmoothFill(ring, L.out_mark, now_us, kPacketUs, &fill))
            {
                const float a = (float)n / (kOutFillTau * (float)kLabRate);
                fill_f += (fill - fill_f) * (a < 1.0f ? a : 1.0f);
            }
            const float c = kOutKp * (fill_f - (float)kOutTarget);
            corr = c > kOutMaxCorr ? kOutMaxCorr : c < -kOutMaxCorr ? -kOutMaxCorr : c;
        }
        const float step = Step();
        for (int i = 0; i < n; i++)
        {
            const float g = gain;
            l[i] = g * Cubic(h[0][0], h[1][0], h[2][0], h[3][0], frac);
            r[i] = g * Cubic(h[0][1], h[1][1], h[2][1], h[3][1], frac);
            if (st == kRun) gain = gain + 1.0f / kFadeSamples < 1.0f ? gain + 1.0f / kFadeSamples : 1.0f;
            else
            {
                gain -= 1.0f / kFadeSamples;
                if (gain <= 0.0f)
                {
                    gain = 0.0f;
                    st   = kIdle;
                    for (int k = i + 1; k < n; k++) l[k] = r[k] = 0.0f;
                    memset(h, 0, sizeof h);
                    return;
                }
            }
            frac += step;
            while (frac >= 1.0f)
            {
                frac -= 1.0f;
                for (int k = 0; k < 3; k++) { h[k][0] = h[k + 1][0]; h[k][1] = h[k + 1][1]; }
                uint32_t f;
                if (st == kRun && ring.Pop(&f)) { h[3][0] = Left(f) / 32768.0f; h[3][1] = Right(f) / 32768.0f; }
                else if (st == kRun)
                {
                    /* nothing more came: hold the last frame and fade out */
                    st = kFade;
                    underruns++;
                }
            }
        }
    }
};

/* ---- IN: the codec at 48 kHz to 44.1 kHz frames in the ring (the Lab's clock) ----- */

struct InStream
{
    float h[4][2] = {};
    float frac = 0.0f;
    float gain = 0.0f;
    bool  was_on = false;

    /* One block of J1 / J2 (-1..1) at now_us, while the host has the stream open. */
    void Process(Link& L, const float* l, const float* r, int n, uint32_t now_us)
    {
        if (!L.in_on.load(std::memory_order_acquire)) { was_on = false; return; }
        if (!was_on)
        {
            memset(h, 0, sizeof h);
            frac = 0.0f;
            gain = 0.0f;
            was_on = true;
        }
        uint32_t pushed = 0;
        L.in_mark.Begin();
        for (int i = 0; i < n; i++)
        {
            for (int k = 0; k < 3; k++) { h[k][0] = h[k + 1][0]; h[k][1] = h[k + 1][1]; }
            h[3][0] = l ? l[i] : 0.0f;
            h[3][1] = r ? r[i] : 0.0f;
            while (frac < 1.0f)
            {
                const float a = gain * Cubic(h[0][0], h[1][0], h[2][0], h[3][0], frac);
                const float b = gain * Cubic(h[0][1], h[1][1], h[2][1], h[3][1], frac);
                L.in.Push(Pack(ToS16(a), ToS16(b)));
                pushed++;
                frac += kInStep;
            }
            frac -= 1.0f;
            gain = gain + 1.0f / kFadeSamples < 1.0f ? gain + 1.0f / kFadeSamples : 1.0f;
        }
        L.in_mark.End(now_us, pushed);
    }
};

inline void Link::OpenIn(bool on)
{
    if (on)
    {
        in.Drain();
        sz_acc = 0;
        sz_primed = false;
        sz_fill = (float)kInTarget;
    }
    in_on.store(on, std::memory_order_release);
}

/* The IN packet: 44 or 45 frames, 44.1 a millisecond on average by the
 * Lab's clock. Nine 44s to a 45 (the FM-1's frame clock taken as the Lab's),
 * and the 45 moved earlier or later when the ring, smoothed, drifts more than
 * kSizerHyst from kInTarget. Silence until the ring has filled once. */
inline int Link::InPacket(uint8_t* p, uint32_t now_us)
{
    uint32_t f[kMaxFrames];
    int n = 44;
    if (++sz_acc >= 10) { sz_acc = 0; n = 45; }
    const int fill = in.Fill();
    if (!sz_primed && fill >= kInTarget)
    {
        sz_primed = true;   /* from the oldest frame: the stream fades in from there */
        sz_fill   = (float)fill;
    }
    if (!sz_primed)
    {
        memset(p, 0, (size_t)n * 4u);
        in_packets.fetch_add(1, std::memory_order_relaxed);
        return n * 4;
    }
    if (fill > kRingFrames - 2 * kMaxFrames)   /* the host stopped asking for a while */
    {
        in.Skip(fill - kInTarget);
        sz_fill = (float)kInTarget;
    }
    float sf;
    if (SmoothFill(in, in_mark, now_us, kBlockUs, &sf)) sz_fill += (sf - sz_fill) * (1.0f / 8.0f);
    if (sz_fill > (float)(kInTarget + kSizerHyst) && n == 44) { n = 45; sz_more++; }
    else if (sz_fill < (float)(kInTarget - kSizerHyst) && n == 45) { n = 44; sz_fewer++; }
    int k = 0;
    while (k < n && in.Pop(&f[k])) k++;
    if (k < n)
    {
        sz_underruns++;
        sz_primed = false;
        const uint32_t last = k ? f[k - 1] : 0u;
        for (; k < n; k++) f[k] = last;   /* hold, don't snap to 0 */
    }
    memcpy(p, f, (size_t)n * 4u);
    in_packets.fetch_add(1, std::memory_order_relaxed);
    return n * 4;
}

} // namespace usbaudio
