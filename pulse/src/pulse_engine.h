/*
 * pulse_engine — the hardware-free core of Pulse, a master clock and eight
 * clocked CV outputs for the Alchemy Lab.
 *
 * Nothing in here knows about the SDK: it takes the two trigger inputs as
 * sample blocks (panel volts / 5, the codec's own scaling) and hands back
 * eight output voltages. The firmware calls Process() from the audio
 * callback; the native tests in ../test drive it the same way.
 *
 * Timeline: one double, `pos_`, counts quarter-note beats since the last
 * reset. Every output derives its cycle count from it as
 *
 *     c = pos * num / den + phase          (num/den = cycles per beat)
 *
 * then swing warps c within each pair of cycles, the integer part of the
 * warped count is the cycle index (euclidean step, chance roll, new random
 * value), and the fractional part is the waveform phase. So a reset lines
 * every output, pattern and random sequence up again, and an external clock
 * that snaps `pos_` on each pulse keeps every multiplication phase-locked.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace pulse {

constexpr int kNumOuts = 8;

/* ---- ratios ---------------------------------------------------------------- */

/* Cycles per quarter-note beat. */
struct Ratio
{
    uint16_t num, den;
    const char* label;
};

constexpr int kNumRatios = 26;
extern const Ratio kRatios[kNumRatios];
constexpr uint8_t kRatioUnity = 13;   /* x1 */

/* ---- waveforms --------------------------------------------------------------- */

enum Wave : uint8_t
{
    kGate = 0,     /* high for WIDTH of the cycle                        */
    kTrig,         /* short pulse at the cycle start, 5-50 ms by WIDTH   */
    kSaw,          /* rising ramp                                        */
    kRamp,         /* falling ramp                                       */
    kTri,          /* triangle, WIDTH = peak position (skew)             */
    kSine,         /* raised cosine, starts at 0                         */
    kEnv,          /* exponential decay from 1, WIDTH = decay length     */
    kStepRand,     /* new random level each cycle, held                  */
    kSmoothRand,   /* glides to a new random level each cycle            */
    kNumWaves
};
extern const char* const kWaveLabels[kNumWaves];

/* ---- external clock ---------------------------------------------------------- */

constexpr int kNumPpqn = 7;
extern const uint16_t kPpqnValues[kNumPpqn];   /* 1 2 4 8 12 24 48 */

enum J2Mode : uint8_t { kJ2Reset = 0, kJ2StartStop = 1, kNumJ2Modes };

constexpr float kBpmMin = 10.0f;
constexpr float kBpmMax = 300.0f;

/* ---- parameters ---------------------------------------------------------------- */

struct OutParams
{
    uint8_t ratio  = kRatioUnity;
    uint8_t wave   = kGate;
    float   width  = 0.5f;   /* 0..1                                   */
    float   level  = 1.0f;   /* 0..1 of 5 V                            */
    float   offset = 0.0f;   /* volts, -5..+5, added after level       */
    float   phase  = 0.0f;   /* 0..1 of a cycle                        */
    uint8_t steps  = 0;      /* euclidean length, 0 = off, 1..32       */
    uint8_t fills  = 0;      /* hits per pattern, clamped to steps     */
    uint8_t rotate = 0;      /* 0..steps-1                             */
    float   chance = 1.0f;   /* 0..1, rolled per hit                   */
    float   swing  = 0.5f;   /* 0.5 (straight) .. 0.75                 */
    float   slew   = 0.0f;   /* 0..1, output glide                     */
};

struct MasterParams
{
    float   bpm      = 120.0f;
    uint8_t ppqn_idx = 5;          /* 24 PPQN */
    uint8_t j2_mode  = kJ2Reset;
};

constexpr int kMaxSteps = 32;

/* True when step `i` of an euclidean pattern (steps, fills, rotate) is a hit.
 * Bresenham form: evenly spread, same necklace as Bjorklund up to rotation. */
bool EuclidHit(int i, int steps, int fills, int rotate);

/* Shape of one cycle, 0..1, before LEVEL / OFFSET. `cycle_s` is the cycle's
 * length in seconds (TRIG's pulse is absolute time); r0/r1 are the held and
 * next random levels for the random shapes. */
float Shape(uint8_t wave, float p, float width, float cycle_s, float r0, float r1);

/* Swing warp of a cycle count: in each pair of cycles the boundary between
 * the two moves from 1 to 2*swing. swing 0.5 is the identity. */
double SwingWarp(double c, float swing);

/* ---- engine --------------------------------------------------------------- */

class Engine
{
  public:
    void Init(float sample_rate);

    OutParams    out[kNumOuts];
    MasterParams master;

    /* Transport. Start/Stop take effect at once; Reset at the next Process. */
    void Start()  { running_ = true; }
    void Stop()   { running_ = false; }
    void Toggle() { running_ = !running_; }
    bool Running() const { return running_; }
    void Reset()  { reset_pending_ = true; }

    /* Tap tempo: `t_s` is the tap time in seconds on any monotonic clock.
     * Two taps set the tempo; up to four are averaged. Ignored while an
     * external clock is driving. */
    void Tap(double t_s);

    /* One audio block. clk / rst are J1 / J2 as the codec sees them
     * (±1.0 = ±5 V at the panel); either may be null. */
    void Process(const float* clk, const float* rst, size_t n);

    /* Results of the last Process(). */
    float Volts(int ch) const { return volts_[ch]; }
    /* 0..1 activity for the LEDs: the shape value, 0 on a rest. */
    float Activity(int ch) const { return act_[ch]; }

    float  Bpm() const;               /* what the clock is running at   */
    bool   ExtLocked() const { return ext_active_; }
    double BeatPos() const { return pos_; }
    double Now() const { return t_; }

    /* Event counters for the LEDs / tests. */
    uint32_t ClockEdges() const { return clk_edges_; }

  private:
    struct ChanState
    {
        int64_t  last_cycle = -1;
        bool     active     = false;   /* this cycle sounds              */
        float    r0 = 0.0f, r1 = 0.0f; /* held / next random level        */
        uint32_t rng        = 1u;
        float    slewed     = 0.0f;
        bool     slew_init  = false;
    };

    void   DoReset();
    void   Advance(double dt);
    void   OnClockEdge();
    void   OnJ2Edge();
    void   EvalOutputs(double dt_block);
    double RateBeatsPerSec() const;

    float  sr_  = 48000.0f;
    double dt_  = 1.0 / 48000.0;
    double t_   = 0.0;     /* seconds since Init           */
    double pos_ = 0.0;     /* beats since reset            */
    bool   running_       = true;
    bool   reset_pending_ = false;
    double reset_t_       = -1.0;  /* when the last reset happened */

    /* input edge detectors (hysteresis 1.0 V / 0.5 V) */
    bool clk_hi_ = false, rst_hi_ = false;

    /* external clock */
    bool     ext_active_  = false;
    double   last_edge_t_ = -1.0;
    double   ext_period_  = 0.0;    /* seconds per pulse, smoothed   */
    uint32_t pulse_count_ = 0;
    double   next_target_ = 0.0;    /* beat pos of the next pulse    */
    uint32_t clk_edges_   = 0;

    /* tap tempo */
    double taps_[4] = {0, 0, 0, 0};
    int    n_taps_  = 0;

    ChanState ch_[kNumOuts];
    float     volts_[kNumOuts] = {};
    float     act_[kNumOuts]   = {};
};

/* ---- persistence (byte layout shared with the firmware's Serializable) ---- */

constexpr size_t kOutBytes   = 24;
constexpr size_t kStateBytes = 8 + kNumOuts * kOutBytes;   /* 200 */

void Pack(const MasterParams& m, const OutParams* o, uint8_t sel, uint8_t* buf);
bool Unpack(const uint8_t* buf, MasterParams& m, OutParams* o, uint8_t& sel);

/* First-boot defaults: a useful starting rack rather than eight identical
 * quarter-note gates. */
void Defaults(MasterParams& m, OutParams* o);

} // namespace pulse
