/*
 * pulse_engine.cpp — see pulse_engine.h.
 */
#include "pulse_engine.h"

#include <cmath>
#include <cstring>

namespace pulse {

const Ratio kRatios[kNumRatios] = {
    {1, 64, "/64"}, {1, 32, "/32"}, {1, 24, "/24"}, {1, 16, "/16"},
    {1, 12, "/12"}, {1, 8, "/8"},   {1, 7, "/7"},   {1, 6, "/6"},
    {1, 5, "/5"},   {1, 4, "/4"},   {1, 3, "/3"},   {1, 2, "/2"},
    {2, 3, "/1.5"}, {1, 1, "x1"},   {3, 2, "x1.5"}, {2, 1, "x2"},
    {3, 1, "x3"},   {4, 1, "x4"},   {5, 1, "x5"},   {6, 1, "x6"},
    {7, 1, "x7"},   {8, 1, "x8"},   {12, 1, "x12"}, {16, 1, "x16"},
    {24, 1, "x24"}, {32, 1, "x32"},
};

const char* const kWaveLabels[kNumWaves] = {
    "Gate", "Trig", "Saw", "Ramp", "Tri", "Sine", "Env", "Random", "Smooth",
};

const char* const kModLabels[kNumModParams] = {
    "Off", "Ratio", "Wave", "Width", "Level", "Offset", "Phase",
    "Steps", "Fills", "Rotate", "Chance", "Swing", "Slew",
};

const uint16_t kPpqnValues[kNumPpqn] = {1, 2, 4, 8, 12, 24, 48};

static constexpr float kTwoPi = 6.28318530718f;
static constexpr float kPi    = 3.14159265359f;

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- pure helpers ------------------------------------------------------------ */

bool EuclidHit(int i, int steps, int fills, int rotate)
{
    if (steps <= 0) return true;
    if (fills <= 0) return false;
    if (fills >= steps) return true;
    int j = (i + rotate) % steps;
    if (j < 0) j += steps;
    return (j * fills) % steps < fills;
}

static uint8_t mod_zone(uint8_t cur, int zones, float amount)
{
    if (zones <= 1) return cur;
    const float n = ((float)cur + 0.5f) / (float)zones + amount;
    int z = (int)floorf(n * (float)zones);
    if (z < 0) z = 0;
    if (z > zones - 1) z = zones - 1;
    return (uint8_t)z;
}

void ApplyMod(OutParams& p, uint8_t param, float a)
{
    switch (param)
    {
        case kModRatio:  p.ratio  = mod_zone(p.ratio, kNumRatios, a); break;
        case kModWave:   p.wave   = mod_zone(p.wave, kNumWaves, a); break;
        case kModWidth:  p.width  = clampf(p.width + a, 0.0f, 1.0f); break;
        case kModLevel:  p.level  = clampf(p.level + a, 0.0f, 1.0f); break;
        case kModOffset: p.offset = clampf(p.offset + a * 10.0f, -5.0f, 5.0f); break;
        case kModPhase:  p.phase  = p.phase + a - floorf(p.phase + a); break;
        case kModSteps:
            p.steps = mod_zone(p.steps, kMaxSteps + 1, a);
            if (p.fills > p.steps) p.fills = p.steps;
            break;
        case kModFills:  p.fills  = mod_zone(p.fills > p.steps ? p.steps : p.fills, p.steps + 1, a); break;
        case kModRotate: if (p.steps) p.rotate = mod_zone(p.rotate % p.steps, p.steps, a); break;
        case kModChance: p.chance = clampf(p.chance + a, 0.0f, 1.0f); break;
        case kModSwing:  p.swing  = clampf(p.swing + a * 0.25f, 0.5f, 0.75f); break;
        case kModSlew:   p.slew   = clampf(p.slew + a, 0.0f, 1.0f); break;
        default: break;
    }
}

double SwingWarp(double c, float swing)
{
    const double s = (double)clampf(swing, 0.5f, 0.75f);
    if (s <= 0.5) return c;
    const double pair = std::floor(c * 0.5);
    const double u    = c - pair * 2.0;          /* 0..2 */
    const double b    = 2.0 * s;                 /* new boundary, 1..1.5 */
    const double w    = (u < b) ? u / b : 1.0 + (u - b) / (2.0 - b);
    return pair * 2.0 + w;
}

float Shape(uint8_t wave, float p, float width, float cycle_s, float r0, float r1)
{
    switch (wave)
    {
        case kGate:
        {
            const float w = clampf(width, 0.01f, 1.0f);
            return (w >= 1.0f || p < w) ? 1.0f : 0.0f;
        }
        case kTrig:
        {
            float len = 0.005f + clampf(width, 0.0f, 1.0f) * 0.045f;
            if (len > 0.5f * cycle_s) len = 0.5f * cycle_s;
            return (p * cycle_s < len) ? 1.0f : 0.0f;
        }
        case kSaw:  return p;
        case kRamp: return 1.0f - p;
        case kTri:
        {
            const float w = clampf(width, 0.001f, 0.999f);
            return p < w ? p / w : (1.0f - p) / (1.0f - w);
        }
        case kSine: return 0.5f - 0.5f * cosf(kTwoPi * p);
        case kEnv:
        {
            const float tau = 0.03f + clampf(width, 0.0f, 1.0f) * 0.6f;
            return expf(-p / tau);
        }
        case kStepRand:   return r1;
        case kSmoothRand: return r0 + (r1 - r0) * (0.5f - 0.5f * cosf(kPi * p));
        default: return 0.0f;
    }
}

static inline uint32_t xorshift(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static inline float rand01(uint32_t& s)
{
    return (float)(xorshift(s) >> 8) * (1.0f / 16777216.0f);
}

static inline bool is_pulse_wave(uint8_t w)
{
    return w == kGate || w == kTrig || w == kEnv;
}

/* ---- engine --------------------------------------------------------------- */

void Engine::Init(float sample_rate)
{
    sr_  = sample_rate;
    dt_  = 1.0 / (double)sample_rate;
    t_   = 0.0;
    clk_hi_ = rst_hi_ = false;
    ext_active_  = false;
    last_edge_t_ = -1.0;
    ext_period_  = 0.0;
    n_taps_      = 0;
    reset_t_     = -1.0;
    for (int c = 0; c < kNumOuts; c++) volts_[c] = act_[c] = 0.0f;
    for (int i = 0; i < kNumCvIns; i++) cv_v_[i] = 0.0f;
    cv_on_ = false;
    DoReset();
}

void Engine::DoReset()
{
    pos_          = 0.0;
    pulse_count_  = 0;
    next_target_  = 1.0 / (double)kPpqnValues[master.ppqn_idx % kNumPpqn];
    reset_t_      = t_;
    for (int c = 0; c < kNumOuts; c++)
    {
        ChanState& s = ch_[c];
        s.last_cycle = -1;
        s.active     = false;
        /* Seeded per output, so chance and random patterns replay from a
         * reset: the same reset gives the same sequence. */
        s.rng = 0x9E3779B9u * (uint32_t)(c + 1) ^ 0x2545F491u;
        if (!s.rng) s.rng = 1u;
        s.r0 = s.r1 = 0.0f;
    }
}

float Engine::Bpm() const
{
    if (ext_active_ && ext_period_ > 0.0)
    {
        const double ppqn = (double)kPpqnValues[master.ppqn_idx % kNumPpqn];
        return (float)(60.0 / (ppqn * ext_period_));
    }
    return clampf(master.bpm, kBpmMin, kBpmMax);
}

double Engine::RateBeatsPerSec() const { return (double)Bpm() / 60.0; }

void Engine::Advance(double dt)
{
    if (!running_ || dt <= 0.0) return;
    pos_ += RateBeatsPerSec() * dt;
    /* Following a clock: never run past the next pulse. A late pulse holds
     * the outputs there; the pulse itself snaps them on. */
    if (ext_active_ && pos_ > next_target_ - 1e-9) pos_ = next_target_ - 1e-9;
}

void Engine::Tap(double t_s)
{
    if (ext_active_) return;
    if (n_taps_ > 0 && (t_s - taps_[n_taps_ - 1] > 2.0 || t_s <= taps_[n_taps_ - 1]))
        n_taps_ = 0;
    if (n_taps_ == 4)
    {
        std::memmove(taps_, taps_ + 1, 3 * sizeof(double));
        n_taps_ = 3;
    }
    taps_[n_taps_++] = t_s;
    if (n_taps_ >= 2)
    {
        const double avg = (taps_[n_taps_ - 1] - taps_[0]) / (double)(n_taps_ - 1);
        if (avg > 0.0) master.bpm = clampf((float)(60.0 / avg), kBpmMin, kBpmMax);
    }
}

void Engine::OnClockEdge()
{
    clk_edges_++;
    const double ppqn = (double)kPpqnValues[master.ppqn_idx % kNumPpqn];

    /* A pulse within 2 ms of a reset is the reset's own downbeat. */
    const bool downbeat = reset_t_ >= 0.0 && (t_ - reset_t_) < 0.002;

    if (!ext_active_)
    {
        /* (Re)acquire: the first pulse after silence is a downbeat. */
        ext_active_ = true;
        ext_period_ = 0.0;
        if (running_) DoReset();
        pulse_count_ = 0;
        next_target_ = pos_ + 1.0 / ppqn;
        last_edge_t_ = t_;
        return;
    }

    const double interval = t_ - last_edge_t_;
    last_edge_t_ = t_;
    if (interval > 0.0)
        ext_period_ = (ext_period_ > 0.0) ? 0.5 * ext_period_ + 0.5 * interval : interval;

    if (!running_)
    {
        /* Stopped: track the tempo only; resume from where we stand. */
        pulse_count_ = (uint32_t)std::floor(pos_ * ppqn + 1e-6);
        next_target_ = (double)(pulse_count_ + 1u) / ppqn;
        return;
    }
    if (downbeat)
    {
        pulse_count_ = 0;
        pos_         = 0.0;
    }
    else
    {
        pulse_count_++;
        pos_ = (double)pulse_count_ / ppqn;
    }
    next_target_ = (double)(pulse_count_ + 1u) / ppqn;
}

void Engine::OnJ2Edge()
{
    if (master.j2_mode == kJ2StartStop)
    {
        Toggle();
        if (running_) DoReset();   /* start from the top */
    }
    else
    {
        DoReset();
    }
}

void Engine::Process(const float* clk, const float* rst, size_t n)
{
    if (reset_pending_)
    {
        reset_pending_ = false;
        DoReset();
    }

    /* Thresholds: 1.0 V up, 0.5 V down (codec scale: 5 V = 1.0). */
    constexpr float kHi = 0.2f, kLo = 0.1f;
    size_t done = 0;
    for (size_t i = 0; i < n; i++)
    {
        bool ce = false, re = false;
        if (clk)
        {
            const float v = clk[i];
            if (!clk_hi_ && v > kHi) { clk_hi_ = true; ce = true; }
            else if (clk_hi_ && v < kLo) clk_hi_ = false;
        }
        if (rst)
        {
            const float v = rst[i];
            if (!rst_hi_ && v > kHi) { rst_hi_ = true; re = true; }
            else if (rst_hi_ && v < kLo) rst_hi_ = false;
        }
        if (ce || re)
        {
            const double d = (double)(i + 1 - done) * dt_;
            t_ += d;
            Advance(d);
            done = i + 1;
            if (re) OnJ2Edge();
            if (ce) OnClockEdge();
        }
    }
    const double d = (double)(n - done) * dt_;
    t_ += d;
    Advance(d);

    /* Lost the external clock? Fall back to the internal tempo. */
    if (ext_active_)
    {
        double timeout = ext_period_ > 0.0 ? 3.0 * ext_period_ : 2.0;
        if (timeout < 0.25) timeout = 0.25;
        if (timeout > 4.0) timeout = 4.0;
        if (t_ - last_edge_t_ > timeout) ext_active_ = false;
    }

    EvalOutputs((double)n * dt_);
}

void Engine::EvalOutputs(double dt_block)
{
    const double rate = RateBeatsPerSec();
    for (int c = 0; c < kNumOuts; c++)
    {
        OutParams  P = out[c];
        ChanState& S = ch_[c];
        if (cv_on_)
            for (int i = 0; i < kNumCvIns; i++)
                if (master.cv[i].param != kModOff && master.cv[i].out == c)
                    ApplyMod(P, master.cv[i].param, cv_v_[i] / 5.0f);
        const Ratio&     r = kRatios[P.ratio < kNumRatios ? P.ratio : kRatioUnity];

        const double cyc  = pos_ * (double)r.num / (double)r.den + (double)P.phase;
        const double w    = SwingWarp(cyc, P.swing);
        const int64_t idx = (int64_t)std::floor(w);
        const float  p    = (float)(w - (double)idx);

        if (idx != S.last_cycle)
        {
            S.last_cycle = idx;
            const int steps = P.steps > kMaxSteps ? kMaxSteps : P.steps;
            const int fills = P.fills > steps ? steps : P.fills;
            bool hit = true;
            if (steps > 0)
            {
                const int step = (int)(idx % steps);
                hit = EuclidHit(step, steps, fills, P.rotate);
            }
            /* The roll happens every cycle, hit or not, so changing the
             * pattern does not reshuffle the chance sequence. */
            const float roll = rand01(S.rng);
            const float nv   = rand01(S.rng);
            S.active = hit && (P.chance >= 1.0f || roll < P.chance);
            if (S.active)
            {
                S.r0 = S.r1;
                S.r1 = nv;
            }
        }

        const float cycle_s = rate > 0.0
            ? (float)((double)r.den / ((double)r.num * rate)) : 1.0f;

        float shape;
        if (!running_ && is_pulse_wave(P.wave))
            shape = 0.0f;
        else if (!S.active)
            shape = (P.wave == kStepRand || P.wave == kSmoothRand) ? S.r1 : 0.0f;
        else
            shape = Shape(P.wave, p, P.width, cycle_s, S.r0, S.r1);

        float v = P.offset + P.level * 5.0f * shape;
        v = clampf(v, -5.0f, 5.0f);

        if (P.slew > 0.0005f && S.slew_init)
        {
            const double tau = 0.001 + (double)(P.slew * P.slew) * 1.0;
            const float  a   = (float)(1.0 - std::exp(-dt_block / tau));
            S.slewed += a * (v - S.slewed);
        }
        else
        {
            S.slewed    = v;
            S.slew_init = true;
        }
        volts_[c] = S.slewed;
        act_[c]   = shape;
    }
}

/* ---- persistence ------------------------------------------------------------ */

static constexpr uint8_t kLayoutVersion = 2;

static inline void put16(uint8_t* b, float v01)
{
    const float    x = clampf(v01, 0.0f, 1.0f);
    const uint16_t q = (uint16_t)lroundf(x * 65535.0f);
    b[0] = (uint8_t)(q & 0xFF);
    b[1] = (uint8_t)(q >> 8);
}

static inline float get16(const uint8_t* b)
{
    return (float)((uint16_t)b[0] | ((uint16_t)b[1] << 8)) / 65535.0f;
}

void Pack(const MasterParams& m, const OutParams* o, uint8_t sel, uint8_t* buf)
{
    std::memset(buf, 0, kStateBytes);
    std::memcpy(buf, &m.bpm, 4);
    buf[4] = m.ppqn_idx;
    buf[5] = m.j2_mode;
    buf[6] = sel;
    buf[7] = kLayoutVersion;
    for (int i = 0; i < kNumCvIns; i++)
    {
        buf[8 + 2 * i]     = m.cv[i].out;
        buf[8 + 2 * i + 1] = m.cv[i].param;
    }
    for (int c = 0; c < kNumOuts; c++)
    {
        uint8_t*         b = buf + kMasterBytes + c * kOutBytes;
        const OutParams& P = o[c];
        b[0] = P.ratio;
        b[1] = P.wave;
        b[2] = P.steps;
        b[3] = P.fills;
        b[4] = P.rotate;
        put16(b + 5, P.width);
        put16(b + 7, P.level);
        put16(b + 9, (P.offset + 5.0f) / 10.0f);
        put16(b + 11, P.phase);
        put16(b + 13, P.chance);
        put16(b + 15, (P.swing - 0.5f) / 0.25f);
        put16(b + 17, P.slew);
    }
}

bool Unpack(const uint8_t* buf, MasterParams& m, OutParams* o, uint8_t& sel)
{
    if (buf[7] != kLayoutVersion) return false;
    float bpm;
    std::memcpy(&bpm, buf, 4);
    if (!(bpm >= kBpmMin && bpm <= kBpmMax)) bpm = 120.0f;
    m.bpm      = bpm;
    m.ppqn_idx = buf[4] < kNumPpqn ? buf[4] : 5;
    m.j2_mode  = buf[5] < kNumJ2Modes ? buf[5] : (uint8_t)kJ2Reset;
    sel        = buf[6] < kNumOuts ? buf[6] : 0;
    for (int i = 0; i < kNumCvIns; i++)
    {
        const uint8_t o = buf[8 + 2 * i], q = buf[8 + 2 * i + 1];
        m.cv[i].out   = o < kNumOuts ? o : 0;
        m.cv[i].param = q < kNumModParams ? q : (uint8_t)kModOff;
    }
    for (int c = 0; c < kNumOuts; c++)
    {
        const uint8_t* b = buf + kMasterBytes + c * kOutBytes;
        OutParams&     P = o[c];
        P.ratio  = b[0] < kNumRatios ? b[0] : kRatioUnity;
        P.wave   = b[1] < kNumWaves ? b[1] : (uint8_t)kGate;
        P.steps  = b[2] <= kMaxSteps ? b[2] : 0;
        P.fills  = b[3] <= P.steps ? b[3] : P.steps;
        P.rotate = (P.steps && b[4] < P.steps) ? b[4] : 0;
        P.width  = get16(b + 5);
        P.level  = get16(b + 7);
        P.offset = get16(b + 9) * 10.0f - 5.0f;
        P.phase  = get16(b + 11);
        P.chance = get16(b + 13);
        P.swing  = 0.5f + get16(b + 15) * 0.25f;
        P.slew   = get16(b + 17);
    }
    return true;
}

void Defaults(MasterParams& m, OutParams* o)
{
    m = MasterParams{};
    for (int c = 0; c < kNumOuts; c++) o[c] = OutParams{};
    /* 1 quarter gates   2 eighth gates   3 sixteenth trigs   4 bar gates */
    o[1].ratio = 15;
    o[2].ratio = 17; o[2].wave = kTrig; o[2].width = 0.1f;
    o[3].ratio = 9;  o[3].width = 0.25f;
    /* 5 two-bar sine, bipolar   6 four-bar triangle, bipolar */
    o[4].ratio = 5;  o[4].wave = kSine; o[4].offset = -2.5f;
    o[5].ratio = 3;  o[5].wave = kTri;  o[5].offset = -2.5f;
    /* 7 tresillo on eighths (3 in 8)   8 random level every beat */
    o[6].ratio = 15; o[6].wave = kTrig; o[6].steps = 8; o[6].fills = 3; o[6].width = 0.1f;
    o[7].wave  = kStepRand;
}

} // namespace pulse
