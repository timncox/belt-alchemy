/*
 * Pulse on the Hermetic Modular Alchemy Lab V2 -- a master clock and eight
 * clocked CV outputs: divisions and multiplications, nine waveforms,
 * euclidean patterns, chance, swing, phase, level, offset and slew per
 * output.
 *
 * The clock and the outputs live in pulse_engine (no SDK, natively tested);
 * this file is the panel: six pots on three layers, three buttons, 102 LEDs,
 * the jacks, presets and the SD firmware picker. See ../DESIGN.md.
 *
 *   OUTPUT   RATIO · WAVE · WIDTH · LEVEL · OFFSET · PHASE     (selected output)
 *   PATTERN  STEPS · FILLS · ROTATE · CHANCE · SWING · SLEW    (hold B3)
 *   MASTER   BPM · CLOCK PPQN · J2 MODE                        (hold B2)
 *   B1       tap: next output; hold + turn P1: pick any output
 *   B2       tap: tap tempo; hold: MASTER layer
 *   B3       tap: start / stop (start is from the top); hold: PATTERN layer
 *   J1       clock in    J2  reset (or start/stop)    J3-J10  outputs 1-8
 *   Settings (B2+B3 2 s): brightness, presets; page 1 = the SD firmware picker
 */
#include <math.h>
#include <string.h>

#include "daisy_seed.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/serializable.h"
#include "alchemy/surface/settings.h"

#include "picker.h"
#include "pulse_engine.h"

#ifndef PULSE_VERSION
#define PULSE_VERSION "0.0.0"
#endif
#ifndef PULSE_GIT_HASH
#define PULSE_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* 24 frames at 48 kHz: the engine and J7-J10 update at 2 kHz, J3-J6 (the
 * I2C DAC) at the 1 kHz control poll. */
static constexpr uint32_t kBlockSize = 24u;

/* This firmware's own preset slot. The family keeps working state in a slot
 * each (Smack 12, Mark 13, Belt 14, Relay 15); Pulse takes 11. */
static constexpr uint8_t kHomeSlot = 11u;

enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };
enum Layer : uint8_t { kLayerOut = 0, kLayerPattern, kLayerMaster };

static constexpr uint32_t kTapMs      = 300u;   /* tap vs hold */
static constexpr uint32_t kAutosaveMs = 5000u;

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab    hw;
static pulse::Engine engine;

/* J3-J6 values from the audio callback, flushed over I2C by the poll. */
static volatile float g_mcp_volts[4] = {0, 0, 0, 0};

/* ---- colours ------------------------------------------------------------ */

using Rgb = LedPanel::Rgb;

static constexpr Rgb kOutCol[pulse::kNumOuts] = {
    {0xFF, 0x30, 0x30}, {0xFF, 0x90, 0x10}, {0xF0, 0xE0, 0x20}, {0x40, 0xFF, 0x40},
    {0x20, 0xE0, 0xE0}, {0x30, 0x70, 0xFF}, {0xA0, 0x40, 0xFF}, {0xFF, 0x40, 0xC0},
};
static constexpr Rgb kMasterCol = {0xFF, 0xB0, 0x40};
static constexpr Rgb kExtCol    = {0x20, 0xC0, 0xFF};
static constexpr Rgb kCatchCol  = {0x30, 0x30, 0x30};
static constexpr Rgb kRunCol    = {0x00, 0x60, 0x00};
static constexpr Rgb kStopCol   = {0x60, 0x00, 0x00};
static constexpr Rgb kDimWhite  = {0x20, 0x20, 0x20};

static Rgb scale(const Rgb& c, float k)
{
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    return {(uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k)};
}

static Rgb tint(const Rgb& c)   /* the PATTERN layer: same hue, paler */
{
    return {(uint8_t)((c.r + 0xFF) / 2), (uint8_t)((c.g + 0xFF) / 2), (uint8_t)((c.b + 0xFF) / 2)};
}

/* ---- persistence -------------------------------------------------------- */

static uint8_t g_sel = 0;           /* selected output, 0..7 */
static bool    g_rearm = true;      /* params changed under the pots */

struct PulseState : public Serializable
{
    size_t SerializedSize() const override { return pulse::kStateBytes; }

    void Serialize(uint8_t* out) const override
    {
        pulse::Pack(engine.master, engine.out, g_sel, out);
    }

    bool Deserialize(const uint8_t* in) override
    {
        pulse::MasterParams m;
        pulse::OutParams    o[pulse::kNumOuts];
        uint8_t             sel;
        if (!pulse::Unpack(in, m, o, sel)) return false;
        engine.master = m;
        for (int c = 0; c < pulse::kNumOuts; c++) engine.out[c] = o[c];
        g_sel   = sel;
        g_rearm = true;
        return true;
    }

    /* 'PLS' + layout version. Bump with pulse_engine's kLayoutVersion. */
    uint32_t SchemaHash() const override { return 0x504C5300u | 0x01u; }
};

/* ---- surfaces ------------------------------------------------------------ */

static Jack jk_clk ("J1",  "Clock",       JackSig::Trig);
static Jack jk_rst ("J2",  "Reset / Run", JackSig::Trig);
static Jack jk_o1  ("J3",  "Out 1",       JackSig::CvBi);
static Jack jk_o2  ("J4",  "Out 2",       JackSig::CvBi);
static Jack jk_o3  ("J5",  "Out 3",       JackSig::CvBi);
static Jack jk_o4  ("J6",  "Out 4",       JackSig::CvBi);
static Jack jk_o5  ("J7",  "Out 5",       JackSig::CvBi);
static Jack jk_o6  ("J8",  "Out 6",       JackSig::CvBi);
static Jack jk_o7  ("J9",  "Out 7",       JackSig::CvBi);
static Jack jk_o8  ("J10", "Out 8",       JackSig::CvBi);

static Manual manual = Manual()
    .Tagline("Master clock and eight clocked CV outputs")
    .Preamble(
        "**Pulse** runs a master clock (10-300 BPM, tap tempo on B2, or follow "
        "a clock on J1) and eight outputs on J3-J10. Each output has a ratio "
        "(/64 to x32), a waveform (gate, trigger, saw, ramp, triangle, sine, "
        "envelope, stepped and smooth random), width, level, offset and "
        "phase; hold B3 for its euclidean pattern, chance, swing and slew. "
        "Tap B1 to step to the next output, or hold B1 and turn the top-left "
        "pot to pick one. Hold B2 for the tempo, the clock input's PPQN and "
        "what J2 does. Tap B3 to start and stop.");

static ControlLoop loop(hw);
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, nullptr);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "pulse_alchemy", "Pulse",
                           PULSE_VERSION, PULSE_GIT_HASH);
static PulseState  state;

/* ---- parameters behind the pots ----------------------------------------------- */

enum class Kind : uint8_t { None, Level, Bipolar, Selector };

struct PotView
{
    Kind  kind  = Kind::None;
    int   zones = 0;      /* Selector */
    float norm  = 0.0f;   /* current value, 0..1 */
    Rgb   col   = {0, 0, 0};
    int   marker = -1;    /* Selector: a zone to mark dimly (x1 on RATIO) */
};

static inline float zone_norm(int i, int n) { return ((float)i + 0.5f) / (float)n; }

static int zone_of(float norm, int n, int cur)
{
    int z = (int)floorf(norm * (float)n);
    if (z < 0) z = 0;
    if (z > n - 1) z = n - 1;
    if (cur >= 0 && z != cur)
    {
        /* Hysteresis: a pot parked on a boundary must not chatter. */
        const float f = norm * (float)n - (float)z;
        if (z > cur && f < 0.15f) z = cur;
        if (z < cur && f > 0.85f) z = cur;
    }
    return z;
}

static Layer g_layer = kLayerOut;
static bool  g_pick  = false;   /* B1 held: P1 picks the output */

static PotView view(Layer layer, uint8_t pot)
{
    PotView v;
    pulse::OutParams&    P = engine.out[g_sel];
    pulse::MasterParams& M = engine.master;
    const Rgb oc = kOutCol[g_sel];
    switch (layer)
    {
        case kLayerOut:
            v.col = oc;
            switch (pot)
            {
                case 0: v.kind = Kind::Selector; v.zones = pulse::kNumRatios;
                        v.norm = zone_norm(P.ratio, v.zones); v.marker = pulse::kRatioUnity; break;
                case 1: v.kind = Kind::Selector; v.zones = pulse::kNumWaves;
                        v.norm = zone_norm(P.wave, v.zones); break;
                case 2: v.kind = Kind::Level;   v.norm = P.width; break;
                case 3: v.kind = Kind::Level;   v.norm = P.level; break;
                case 4: v.kind = Kind::Bipolar; v.norm = (P.offset + 5.0f) / 10.0f; break;
                case 5: v.kind = Kind::Level;   v.norm = P.phase; break;
            }
            break;
        case kLayerPattern:
            v.col = tint(oc);
            switch (pot)
            {
                case 0: v.kind = Kind::Selector; v.zones = pulse::kMaxSteps + 1;
                        v.norm = zone_norm(P.steps, v.zones); break;
                case 1: v.kind = Kind::Selector; v.zones = P.steps + 1;
                        v.norm = zone_norm(P.fills > P.steps ? P.steps : P.fills, v.zones); break;
                case 2: v.kind = Kind::Selector; v.zones = P.steps ? P.steps : 1;
                        v.norm = zone_norm(P.steps ? P.rotate % P.steps : 0, v.zones); break;
                case 3: v.kind = Kind::Level; v.norm = P.chance; break;
                case 4: v.kind = Kind::Level; v.norm = (P.swing - 0.5f) / 0.25f; break;
                case 5: v.kind = Kind::Level; v.norm = P.slew; break;
            }
            break;
        case kLayerMaster:
            v.col = kMasterCol;
            switch (pot)
            {
                case 0: v.kind = Kind::Level;
                        v.norm = (M.bpm - pulse::kBpmMin) / (pulse::kBpmMax - pulse::kBpmMin); break;
                case 1: v.kind = Kind::Selector; v.zones = pulse::kNumPpqn;
                        v.norm = zone_norm(M.ppqn_idx, v.zones); break;
                case 2: v.kind = Kind::Selector; v.zones = pulse::kNumJ2Modes;
                        v.norm = zone_norm(M.j2_mode, v.zones); break;
                default: break;
            }
            break;
    }
    return v;
}

/* Apply a caught pot's position. Returns true if anything changed. */
static bool apply(Layer layer, uint8_t pot, float x)
{
    pulse::OutParams&    P = engine.out[g_sel];
    pulse::MasterParams& M = engine.master;
    const PotView        v = view(layer, pot);
    if (v.kind == Kind::None) return false;

    if (v.kind == Kind::Selector)
    {
        const int cur = (int)floorf(v.norm * (float)v.zones);
        const int z   = zone_of(x, v.zones, cur);
        if (z == cur) return false;
        switch (layer)
        {
            case kLayerOut:
                if (pot == 0) P.ratio = (uint8_t)z;
                if (pot == 1) P.wave  = (uint8_t)z;
                break;
            case kLayerPattern:
                if (pot == 0)
                {
                    P.steps = (uint8_t)z;
                    if (P.fills > P.steps) P.fills = P.steps;
                    if (P.steps == 0 || P.rotate >= P.steps) P.rotate = 0;
                    /* A fresh pattern starts half full, so turning STEPS up
                     * from off is audible at once. */
                    if (P.steps && P.fills == 0) P.fills = (uint8_t)((P.steps + 1) / 2);
                }
                if (pot == 1) P.fills  = (uint8_t)z;
                if (pot == 2) P.rotate = (uint8_t)z;
                break;
            case kLayerMaster:
                if (pot == 1)
                {
                    /* A new PPQN re-counts the incoming pulses from a
                     * downbeat rather than jumping mid-bar. */
                    M.ppqn_idx = (uint8_t)z;
                    engine.Reset();
                }
                if (pot == 2) M.j2_mode  = (uint8_t)z;
                break;
        }
        return true;
    }

    switch (layer)
    {
        case kLayerOut:
            if (pot == 2) P.width  = x;
            if (pot == 3) P.level  = x;
            if (pot == 4) P.offset = x * 10.0f - 5.0f;
            if (pot == 5) P.phase  = x;
            break;
        case kLayerPattern:
            if (pot == 3) P.chance = x;
            if (pot == 4) P.swing  = 0.5f + x * 0.25f;
            if (pot == 5) P.slew   = x;
            break;
        case kLayerMaster:
            if (pot == 0)
            {
                const float bpm = roundf(pulse::kBpmMin + x * (pulse::kBpmMax - pulse::kBpmMin));
                if (bpm == M.bpm) return false;
                M.bpm = bpm;
            }
            break;
    }
    return true;
}

/* ---- pot catch -------------------------------------------------------------- */

/*
 * The pots are shared by eight outputs, three layers and every firmware on
 * the card, so a pot only takes over once it reaches (or crosses) the value
 * it would change. Until then its ring shows a dim pip where it physically
 * sits.
 */
struct Catch
{
    bool  caught = false;
    float sign   = 0.0f;
    float last   = 0.0f;
};
static Catch g_catch[kNumPots];
static float g_phys[kNumPots];

static void arm_catches(void)
{
    for (uint8_t p = 0; p < kNumPots; p++)
    {
        const PotView v = view(g_layer, p);
        const float   d = g_phys[p] - v.norm;
        g_catch[p].caught = v.kind != Kind::None && fabsf(d) < 0.02f;
        g_catch[p].sign   = d >= 0.0f ? 1.0f : -1.0f;
        g_catch[p].last   = g_phys[p];
    }
}

/* ---- buttons ------------------------------------------------------------------- */

struct Btn
{
    bool     down  = false;
    uint32_t since = 0;
};
static Btn  g_b1, g_b2, g_b3;
static bool g_chord     = false;   /* B2+B3 together: Settings' chord */
static bool g_b3_moved  = false;   /* a pot moved while B3 was held   */
static bool g_b1_picked = false;
static float g_pick_last = -1.0f;

static bool     g_dirty       = false;
static uint32_t g_dirty_since = 0;

static void mark_dirty(uint32_t now)
{
    g_dirty       = true;
    g_dirty_since = now;
}

static void OnPoll(uint32_t now)
{
    /* J3-J6 over I2C, only when a code would change (10 V / 4096). */
    {
        static float sent[4] = {99, 99, 99, 99};
        bool         any     = false;
        for (int c = 0; c < 4; c++)
        {
            const float v = g_mcp_volts[c];
            if (fabsf(v - sent[c]) > 0.002f)
            {
                hw.cv_jacks[c].StageVolts(v);
                sent[c] = v;
                any     = true;
            }
        }
        if (any) hw.FlushCvOutputs();
    }

    static bool was_settings = false;
    if (settings.IsActive())
    {
        g_b1 = g_b2 = g_b3 = Btn{};
        g_chord = g_pick = false;
        was_settings = true;
        return;
    }
    if (was_settings)
    {
        /* The pots moved under Settings (and a preset may have loaded). */
        was_settings = false;
        g_rearm      = true;
    }

    const bool b1 = hw.buttons[kButtonB1].Pressed();
    const bool b2 = hw.buttons[kButtonB2].Pressed();
    const bool b3 = hw.buttons[kButtonB3].Pressed();

    if (b2 && b3) g_chord = true;

    /* B1: tap = next output, hold = pick with P1. */
    if (b1 && !g_b1.down) { g_b1.down = true; g_b1.since = now; g_b1_picked = false; g_pick_last = -1.0f; }
    if (b1 && now - g_b1.since >= kTapMs) g_pick = true;
    if (!b1 && g_b1.down)
    {
        if (now - g_b1.since < kTapMs && !g_chord)
        {
            g_sel   = (uint8_t)((g_sel + 1) % pulse::kNumOuts);
            g_rearm = true;
            mark_dirty(now);
        }
        g_b1.down = false;
        if (g_pick) g_rearm = true;
        g_pick = false;
    }

    /* B2: tap = tap tempo, hold = MASTER. */
    if (b2 && !g_b2.down) { g_b2.down = true; g_b2.since = now; }
    if (!b2 && g_b2.down)
    {
        if (now - g_b2.since < kTapMs && !g_chord)
        {
            engine.Tap((double)System::GetUs() * 1e-6);
            mark_dirty(now);
        }
        g_b2.down = false;
    }

    /* B3: tap = start / stop, hold = PATTERN. */
    if (b3 && !g_b3.down) { g_b3.down = true; g_b3.since = now; g_b3_moved = false; }
    if (!b3 && g_b3.down)
    {
        if (now - g_b3.since < kTapMs && !g_chord && !g_b3_moved)
        {
            if (engine.Running()) engine.Stop();
            else { engine.Reset(); engine.Start(); }
        }
        g_b3.down = false;
    }

    if (!b1 && !b2 && !b3) g_chord = false;

    Layer want = kLayerOut;
    if (!g_chord)
    {
        if (b3) want = kLayerPattern;
        else if (b2 && now - g_b2.since >= kTapMs) want = kLayerMaster;
    }
    if (want != g_layer)
    {
        g_layer = want;
        g_rearm = true;
    }
}

/* ---- control frame ------------------------------------------------------------- */

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();
    for (uint8_t p = 0; p < kNumPots; p++) g_phys[p] = hw.pots[p].Value();

    if (g_pick)
    {
        /* P1 jumps straight to an output, but only once it moves. */
        if (g_pick_last < 0.0f) g_pick_last = g_phys[0];
        if (fabsf(g_phys[0] - g_pick_last) > 0.01f || g_b1_picked)
        {
            g_b1_picked = true;
            const int z = zone_of(g_phys[0], pulse::kNumOuts, g_sel);
            if (z != g_sel)
            {
                g_sel = (uint8_t)z;
                mark_dirty(now);
            }
        }
        return;
    }

    if (g_rearm)
    {
        g_rearm = false;
        arm_catches();
    }

    for (uint8_t p = 0; p < kNumPots; p++)
    {
        Catch&        c = g_catch[p];
        const PotView v = view(g_layer, p);
        if (v.kind == Kind::None) continue;
        const float x = g_phys[p];
        if (!c.caught)
        {
            const float d = x - v.norm;
            if (fabsf(d) < 0.02f || (d >= 0.0f ? 1.0f : -1.0f) != c.sign)
            {
                c.caught = true;
                c.last   = x;
            }
            continue;
        }
        if (fabsf(x - c.last) < 0.004f) continue;   /* ADC noise */
        c.last = x;
        if (apply(g_layer, p, x))
        {
            mark_dirty(now);
            if (g_layer == kLayerPattern) g_b3_moved = true;
            /* STEPS reshapes FILLS and ROTATE: re-catch those two. */
            if (g_layer == kLayerPattern && p == 0)
                for (uint8_t q = 1; q <= 2; q++)
                {
                    const PotView w = view(g_layer, q);
                    g_catch[q].caught = false;
                    g_catch[q].sign   = (g_phys[q] - w.norm) >= 0.0f ? 1.0f : -1.0f;
                }
        }
    }

    /* Autosave the working state to the home slot once hands are off. */
    if (g_dirty && now - g_dirty_since > kAutosaveMs && !g_b1.down && !g_b2.down
        && !g_b3.down && !picker::Busy())
    {
        g_dirty = false;
        presets.Save(kHomeSlot);
    }
}

/* ---- LEDs ----------------------------------------------------------------------- */

static void ring_led(uint8_t pot, int i, const Rgb& c)
{
    const ArcGeometry& geo = hw.Arc();
    if (i < 0) i = 0;
    if (i > geo.arc_leds - 1) i = geo.arc_leds - 1;
    hw.leds.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)i, 12.0f),
                          hw.leds.ScaleGlobal(c));
}

static int arc_index(float norm)
{
    const int n = hw.Arc().arc_leds;
    return (int)lroundf(norm * (float)(n - 1));
}

static int zone_index(int z, int zones)
{
    const int n = hw.Arc().arc_leds;
    if (zones <= 1) return 0;
    return (int)lroundf((float)z * (float)(n - 1) / (float)(zones - 1));
}

static void draw_view(uint8_t pot, const PotView& v)
{
    const int n = hw.Arc().arc_leds;
    switch (v.kind)
    {
        case Kind::None: return;
        case Kind::Level:
        {
            const int k = (int)lroundf(v.norm * (float)n);
            for (int i = 0; i < k; i++) ring_led(pot, i, v.col);
            if (k == 0) ring_led(pot, 0, scale(v.col, 0.15f));
            break;
        }
        case Kind::Bipolar:
        {
            const int mid = (n - 1) / 2;
            const int at  = arc_index(v.norm);
            const int lo = at < mid ? at : mid, hi = at < mid ? mid : at;
            for (int i = lo; i <= hi; i++) ring_led(pot, i, v.col);
            ring_led(pot, mid, kDimWhite);
            break;
        }
        case Kind::Selector:
        {
            const int z = (int)floorf(v.norm * (float)v.zones);
            if (v.zones <= n)
                for (int i = 0; i < v.zones; i++) ring_led(pot, zone_index(i, v.zones), scale(v.col, 0.08f));
            if (v.marker >= 0) ring_led(pot, zone_index(v.marker, v.zones), kDimWhite);
            ring_led(pot, zone_index(z, v.zones), v.col);
            break;
        }
    }
}

static void OnRender(uint32_t /*t_ms*/)
{
    if (settings.IsActive()) return;   /* Settings owns the panel */
    LedPanel& L = hw.leds;

    if (g_pick)
    {
        /* P1: the eight outputs, each in its colour, the chosen one bright
         * and pulsing with its output. */
        for (int c = 0; c < pulse::kNumOuts; c++)
        {
            const float k = c == g_sel ? 0.4f + 0.6f * engine.Activity(c) : 0.12f;
            ring_led(0, zone_index(c, pulse::kNumOuts), scale(kOutCol[c], k));
        }
    }
    else
    {
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            const PotView v = view(g_layer, p);
            draw_view(p, v);
            if (v.kind != Kind::None && !g_catch[p].caught)
                ring_led(p, arc_index(g_phys[p]), kCatchCol);
        }
        /* MASTER: the BPM ring flashes on the beat. */
        if (g_layer == kLayerMaster)
        {
            const double ph = engine.BeatPos() - floor(engine.BeatPos());
            if (ph < 0.12 && engine.Running())
                ring_led(0, hw.Arc().arc_leds - 1, engine.ExtLocked() ? kExtCol : Rgb{0xFF, 0xFF, 0xFF});
        }
    }

    /* B1: the selected output's colour, lit by its activity. */
    L.SetButtonPair(kButtonB1,
                    L.ScaleGlobal(scale(kOutCol[g_sel], 0.2f + 0.8f * engine.Activity(g_sel))));

    /* B2: the beat; cyan while following J1. */
    {
        const double ph   = engine.BeatPos() - floor(engine.BeatPos());
        const Rgb    base = engine.ExtLocked() ? kExtCol : kMasterCol;
        const bool   on   = engine.Running() && ph < 0.15;
        L.SetButtonPair(kButtonB2, L.ScaleGlobal(scale(base, on ? 1.0f : 0.1f)));
    }

    /* B3: running (green) / stopped (red); pale while PATTERN is up. */
    L.SetButtonPair(kButtonB3, L.ScaleGlobal(g_layer == kLayerPattern
                                                 ? tint(engine.Running() ? kRunCol : kStopCol)
                                                 : (engine.Running() ? kRunCol : kStopCol)));
}

/* ---- audio ----------------------------------------------------------------- */

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           n)
{
    /* J1 / J2 are the codec inputs: sample-accurate clock and reset edges. */
    engine.Process(in[0], in[1], n);

    for (size_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;

    for (int c = 0; c < 4; c++) g_mcp_volts[c] = engine.Volts(c);   /* J3-J6 */
    hw.cv_jacks[4].SetVolts(engine.Volts(4));                       /* J7  */
    hw.cv_jacks[5].SetVolts(engine.Volts(5));                       /* J8  */
    hw.cv_jacks[6].SetVolts(engine.Volts(6));                       /* J9  */
    hw.cv_jacks[7].SetVolts(engine.Volts(7));                       /* J10 */
}

/* ---- main ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);

    engine.Init(hw.SampleRate());
    pulse::Defaults(engine.master, engine.out);

    /* Every jack J3-J10 is an output. */
    for (uint8_t j = 0; j < kNumCvJacks; j++)
    {
        hw.cv_jacks[j].SetVolts(0.0f);
        hw.cv_jacks[j].EnableCvOutput();
    }

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    host.Product("Pulse")
        .BootSlot(kHomeSlot)
        .Jacks(jk_clk, jk_rst, jk_o1, jk_o2, jk_o3, jk_o4, jk_o5, jk_o6, jk_o7, jk_o8)
        .Attach(manual)
        .Extend(fs_ext);

    presets.Manage(state);
    presets.Manage(settings);
    presets.Init();
    const bool had_home = presets.HasValid(kHomeSlot);
    presets.BootLoad();
    if (had_home) presets.Load(kHomeSlot);

    for (int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for (uint8_t p = 0; p < kNumPots; p++) g_phys[p] = hw.pots[p].Value();
    g_rearm = true;

    hw.StartAudio(AudioCallback);

    loop.Use(settings)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);
    loop.Use(host);

    for (;;) loop.Tick();
}
