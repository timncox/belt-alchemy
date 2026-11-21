/*
 * TETHER on the Hermetic Modular Alchemy Lab V2 -- MIDI to CV. A USB-MIDI
 * device on the front USB-C (the Lab is the host): two voices with pitch
 * and gate, MIDI clock, reset, run, accents, velocities and drum triggers.
 * Made for PROTECTOR's PARTS setting (each part on its own channel: 1 KICK
 * 2 SNARE 3 HATS 4 PERC 5 BASS 6 LEAD 7 PAD 8 FX, section changes as CC 85
 * on channel 16), and fine with any USB-MIDI keyboard.
 *
 * The MIDI side lives in tether_engine (no SDK, natively tested); this file
 * is the panel: six pots on three layers, three buttons, the jacks,
 * presets and the SD firmware picker.
 *
 *   VOICE   PART · GATE · GLIDE · OCTAVE · TRANSPOSE · NOTES    (voice A or B)
 *   JACKS   J3 · J4 · J5 · J6 · J7 · J8                           (hold B3)
 *   MASTER  CAL OFFSET · CAL SCALE · TRIG LENGTH · ACCENT · BEND · RESET ON  (hold B2)
 *   B1      tap: voice A / B; with B2 held: the calibration reference (off, 1 V, 3 V)
 *   B2      tap: all notes off; hold: MASTER
 *   B3      tap: a test note on the voice; hold: JACKS
 *   J9 J10  pitch A, pitch B (1 V/oct, 24-bit)   J3-J8  as set on JACKS
 *   Hold B1 while powering up: Mac mode (USB device, HostLink) instead of host.
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

#include "midi_host.h"
#include "picker.h"
#include "tether_engine.h"

#ifndef TETHER_VERSION
#define TETHER_VERSION "0.0.0"
#endif
#ifndef TETHER_GIT_HASH
#define TETHER_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* 24 frames at 48 kHz: MIDI is taken and J7-J10 updated at 2 kHz, J3-J6
 * (the I2C DAC) at the 1 kHz control poll. */
static constexpr uint32_t kBlockSize = 24u;

/* Working state's preset slot (Pulse 11, Smack 12, Mark 13, Belt 14, Relay 15). */
static constexpr uint8_t kHomeSlot = 10u;

enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };
enum Layer : uint8_t { kLayerVoice = 0, kLayerJacks, kLayerMaster };

static constexpr uint32_t kTapMs      = 300u;
static constexpr uint32_t kAutosaveMs = 5000u;
static constexpr uint32_t kHostWaitMs = 2300u;   /* as Belt: the port settles first */
static constexpr uint32_t kTestMs     = 300u;

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab     hw;
static tether::Engine engine;

static volatile float g_mcp_volts[4] = {0, 0, 0, 0};   /* J3-J6, flushed by the poll */

/* Panel -> audio thread: test notes, all notes off. */
enum : uint32_t { kCmdTestOn = 1, kCmdTestOff, kCmdPanic };
static tether::Ring<16> g_cmd;

static bool     g_mac_mode = false;   /* B1 at power-up: USB device, no host */
static bool     g_host_up  = false;
static uint32_t g_boot_ms  = 0;
static uint8_t  g_stage    = 0;

/* ---- colours ------------------------------------------------------------ */

using Rgb = LedPanel::Rgb;

static constexpr Rgb kVoiceCol[tether::kVoices] = {{0xFF, 0x80, 0x10}, {0x20, 0xD0, 0xFF}};
static constexpr Rgb kMasterCol = {0xFF, 0xB0, 0x40};
static constexpr Rgb kCalCol    = {0xFF, 0xFF, 0xFF};
static constexpr Rgb kCatchCol  = {0x30, 0x30, 0x30};
static constexpr Rgb kDimWhite  = {0x20, 0x20, 0x20};
static constexpr Rgb kGreen     = {0x00, 0xC0, 0x20};
static constexpr Rgb kAmber     = {0xC0, 0x70, 0x00};
static constexpr Rgb kRed       = {0xC0, 0x00, 0x00};
static constexpr Rgb kBlue      = {0x10, 0x30, 0xFF};
static constexpr Rgb kPartCol[8] = {
    {0xFF, 0x30, 0x30}, {0xFF, 0x90, 0x10}, {0xF0, 0xE0, 0x20}, {0x40, 0xFF, 0x40},
    {0x20, 0xE0, 0xE0}, {0x30, 0x70, 0xFF}, {0xA0, 0x40, 0xFF}, {0xFF, 0x40, 0xC0},
};

static Rgb scale(const Rgb& c, float k)
{
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    return {(uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k)};
}

/* A jack's job, in a colour of its kind. */
static Rgb fn_col(uint8_t fn)
{
    using namespace tether;
    if (fn == kFnGateA || fn == kFnAccA || fn == kFnVelA) return kVoiceCol[0];
    if (fn == kFnGateB || fn == kFnAccB || fn == kFnVelB) return kVoiceCol[1];
    if (fn >= kFnClk24 && fn <= kFnBar) return {0xFF, 0xFF, 0xC0};
    if (fn == kFnReset) return {0xFF, 0x10, 0x10};
    if (fn == kFnRun) return {0x10, 0xFF, 0x10};
    if (fn == kFnSection) return {0xB0, 0x40, 0xFF};
    if (fn >= kFnTrig1 && fn < kNumJackFns) return kPartCol[fn - kFnTrig1];
    return kDimWhite;
}

/* ---- persistence -------------------------------------------------------- */

static uint8_t g_sel   = 0;      /* voice on the VOICE layer, 0 = A */
static bool    g_rearm = true;

struct TetherState : public Serializable
{
    size_t SerializedSize() const override { return tether::kStateBytes; }
    void   Serialize(uint8_t* out) const override { tether::Pack(engine.master, engine.voice, g_sel, out); }
    bool   Deserialize(const uint8_t* in) override
    {
        tether::MasterParams m;
        tether::VoiceParams  v[tether::kVoices];
        uint8_t              sel;
        if (!tether::Unpack(in, m, v, sel)) return false;
        engine.master = m;
        for (int i = 0; i < tether::kVoices; i++) engine.voice[i] = v[i];
        g_sel   = sel;
        g_rearm = true;
        return true;
    }
    /* 'TTH' + layout version. */
    uint32_t SchemaHash() const override { return 0x54544800u | tether::kLayoutVersion; }
};

/* ---- surfaces ------------------------------------------------------------ */

static Jack jk_j1 ("J1",  "Not used",          JackSig::Trig);
static Jack jk_j2 ("J2",  "Not used",          JackSig::Trig);
static Jack jk_j3 ("J3",  "Gate A (JACKS P1)", JackSig::CvBi);
static Jack jk_j4 ("J4",  "Gate B (JACKS P2)", JackSig::CvBi);
static Jack jk_j5 ("J5",  "Clock (JACKS P3)",  JackSig::CvBi);
static Jack jk_j6 ("J6",  "Reset (JACKS P4)",  JackSig::CvBi);
static Jack jk_j7 ("J7",  "Trig 1 (JACKS P5)", JackSig::CvBi);
static Jack jk_j8 ("J8",  "Trig 2 (JACKS P6)", JackSig::CvBi);
static Jack jk_j9 ("J9",  "Pitch A, 1 V/oct",  JackSig::CvBi);
static Jack jk_j10("J10", "Pitch B, 1 V/oct",  JackSig::CvBi);

static Manual manual = Manual()
    .Tagline("MIDI to CV from a USB-MIDI device")
    .Preamble(
        "**TETHER** turns a USB-MIDI device on the front USB-C into CV. Voice A "
        "and voice B each follow a MIDI channel: pitch at 1 V/oct on J9 and J10, "
        "gates on J3 and J4. J5 puts out a 16th-note clock, J6 a reset trig on "
        "start, J7 and J8 a trigger for channels 1 and 2. Hold B3 to give J3-J8 "
        "any job: gates, clock at 24 PPQN, 16ths, 8ths or quarters, bars, reset, "
        "run, PROTECTOR's section as a voltage, accents, velocities, or a "
        "trigger for channel 1-8. Tap B1 for voice A or B; its pots set the "
        "channel, gate, accent or velocity gate, glide, octave, transpose and "
        "note priority. Hold B2 for calibration, trigger length, the accent "
        "velocity, bend range and when RESET fires. A powered USB hub between "
        "the Lab and the keyboard gives it power; the Lab's port gives none.");

static ControlLoop loop(hw);
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, nullptr);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "tether_alchemy", "TETHER", TETHER_VERSION, TETHER_GIT_HASH);
static TetherState state;

/* ---- parameters behind the pots ----------------------------------------- */

enum class Kind : uint8_t { None, Level, Bipolar, Selector };

struct PotView
{
    Kind  kind   = Kind::None;
    int   zones  = 0;
    float norm   = 0.0f;
    Rgb   col    = {0, 0, 0};
    int   marker = -1;
};

static inline float zone_norm(int i, int n) { return ((float)i + 0.5f) / (float)n; }

static int zone_of(float norm, int n, int cur)
{
    int z = (int)floorf(norm * (float)n);
    if (z < 0) z = 0;
    if (z > n - 1) z = n - 1;
    if (cur >= 0 && z != cur)
    {
        const float f = norm * (float)n - (float)z;
        if (z > cur && f < 0.15f) z = cur;
        if (z < cur && f > 0.85f) z = cur;
    }
    return z;
}

static Layer g_layer = kLayerVoice;

static PotView view(Layer layer, uint8_t pot)
{
    using namespace tether;
    PotView             v;
    VoiceParams&        P = engine.voice[g_sel];
    const MasterParams& M = engine.master;
    switch (layer)
    {
        case kLayerVoice:
            v.col = kVoiceCol[g_sel];
            switch (pot)
            {
                case 0: v.kind = Kind::Selector; v.zones = kNumChans; v.norm = zone_norm(P.chan, v.zones);
                        v.marker = 4 + g_sel; break;   /* PROTECTOR's BASS / LEAD */
                case 1: v.kind = Kind::Selector; v.zones = kNumGateModes; v.norm = zone_norm(P.gate_mode, v.zones); break;
                case 2: v.kind = Kind::Level; v.norm = P.glide; break;
                case 3: v.kind = Kind::Selector; v.zones = kOctMax - kOctMin + 1;
                        v.norm = zone_norm(P.octave - kOctMin, v.zones); v.marker = -kOctMin; break;
                case 4: v.kind = Kind::Selector; v.zones = kTransMax - kTransMin + 1;
                        v.norm = zone_norm(P.transpose - kTransMin, v.zones); v.marker = -kTransMin; break;
                case 5: v.kind = Kind::Selector; v.zones = kNumNoteModes; v.norm = zone_norm(P.note_mode, v.zones); break;
            }
            break;
        case kLayerJacks:
            v.kind  = Kind::Selector;
            v.zones = kNumJackFns;
            v.norm  = zone_norm(M.jack[pot], v.zones);
            v.col   = fn_col(M.jack[pot]);
            break;
        case kLayerMaster:
            v.col = kMasterCol;
            switch (pot)
            {
                case 0: v.kind = Kind::Bipolar; v.col = kVoiceCol[g_sel];
                        v.norm = 0.5f + 0.5f * M.cal_off[g_sel] / kCalOffMax; break;
                case 1: v.kind = Kind::Bipolar; v.col = kVoiceCol[g_sel];
                        v.norm = 0.5f + 0.5f * M.cal_scale[g_sel] / kCalScaleMax; break;
                case 2: v.kind = Kind::Level; v.norm = (M.trig_ms - kTrigMsMin) / (kTrigMsMax - kTrigMsMin); break;
                case 3: v.kind = Kind::Level; v.norm = (float)(M.accent - 64) / 63.0f; break;
                case 4: v.kind = Kind::Selector; v.zones = kBendMax + 1; v.norm = zone_norm(M.bend, v.zones); v.marker = 2; break;
                case 5: v.kind = Kind::Selector; v.zones = kNumResetOn; v.norm = zone_norm(M.reset_on, v.zones); break;
            }
            break;
    }
    return v;
}

static bool apply(Layer layer, uint8_t pot, float x)
{
    using namespace tether;
    VoiceParams&  P = engine.voice[g_sel];
    MasterParams& M = engine.master;
    const PotView v = view(layer, pot);
    if (v.kind == Kind::None) return false;
    if (v.kind == Kind::Selector)
    {
        const int cur = (int)floorf(v.norm * (float)v.zones);
        const int z   = zone_of(x, v.zones, cur);
        if (z == cur) return false;
        switch (layer)
        {
            case kLayerVoice:
                if (pot == 0) { P.chan = (uint8_t)z; g_cmd.Push(kCmdPanic); }   /* nothing left hanging */
                if (pot == 1) P.gate_mode = (uint8_t)z;
                if (pot == 3) P.octave = (int8_t)(z + kOctMin);
                if (pot == 4) P.transpose = (int8_t)(z + kTransMin);
                if (pot == 5) P.note_mode = (uint8_t)z;
                break;
            case kLayerJacks: M.jack[pot] = (uint8_t)z; break;
            case kLayerMaster:
                if (pot == 4) M.bend = (uint8_t)z;
                if (pot == 5) M.reset_on = (uint8_t)z;
                break;
        }
        return true;
    }
    switch (layer)
    {
        case kLayerVoice:
            if (pot == 2) P.glide = x;
            break;
        case kLayerMaster:
            if (pot == 0) M.cal_off[g_sel] = (x - 0.5f) * 2.0f * kCalOffMax;
            if (pot == 1) M.cal_scale[g_sel] = (x - 0.5f) * 2.0f * kCalScaleMax;
            if (pot == 2) M.trig_ms = roundf(kTrigMsMin + x * (kTrigMsMax - kTrigMsMin));
            if (pot == 3) M.accent = (uint8_t)lroundf(64.0f + x * 63.0f);
            break;
        default: break;
    }
    return true;
}

/* ---- pot catch (as Pulse) ------------------------------------------------- */

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
static Btn      g_b1, g_b2, g_b3;
static bool     g_chord   = false;
static bool     g_b2_used = false;   /* B1 tapped (reference) while B2 was held */
static bool     g_b3_used = false;   /* a pot moved while B3 was held */
static uint32_t g_test_until = 0;
static int      g_test_voice = -1;

static bool     g_dirty       = false;
static uint32_t g_dirty_since = 0;
static void mark_dirty(uint32_t now) { g_dirty = true; g_dirty_since = now; }

static void OnPoll(uint32_t now)
{
    /* J3-J6 over I2C, only when a code would change. */
    {
        static float sent[4] = {99, 99, 99, 99};
        bool         any     = false;
        for (int c = 0; c < 4; c++)
        {
            const float v = g_mcp_volts[c];
            if (fabsf(v - sent[c]) > 0.001f)
            {
                hw.cv_jacks[c].StageVolts(v);
                sent[c] = v;
                any     = true;
            }
        }
        if (any) hw.FlushCvOutputs();
    }

    /* The USB host, once the port has settled. A device that goes away
     * leaves nothing held. */
    if (!g_mac_mode)
    {
        if (!g_host_up && now - g_boot_ms > kHostWaitMs)
        {
            midihost::Init();
            g_host_up = true;
        }
        if (g_host_up)
        {
            midihost::Poll(now);
            const uint8_t st = midihost::Stage();
            if (g_stage == 3 && st != 3) g_cmd.Push(kCmdPanic);
            g_stage = st;
        }
    }

    if (g_test_voice >= 0 && (int32_t)(now - g_test_until) >= 0)
    {
        g_cmd.Push(kCmdTestOff | (uint32_t)g_test_voice << 8);
        g_test_voice = -1;
    }

    static bool was_settings = false;
    static bool deaf         = false;
    if (settings.IsActive())
    {
        g_b1 = g_b2 = g_b3 = Btn{};
        g_chord = false;
        was_settings = true;
        return;
    }
    if (was_settings)
    {
        was_settings = false;
        g_rearm      = true;
        deaf         = true;
    }

    const bool b1 = hw.buttons[kButtonB1].Pressed();
    const bool b2 = hw.buttons[kButtonB2].Pressed();
    const bool b3 = hw.buttons[kButtonB3].Pressed();
    if (deaf)
    {
        if (b1 || b2 || b3) return;
        deaf = false;
    }
    if (b2 && b3) g_chord = true;

    /* B1: tap = the other voice; with B2 held = the calibration reference. */
    if (b1 && !g_b1.down) { g_b1.down = true; g_b1.since = now; }
    if (!b1 && g_b1.down)
    {
        if (now - g_b1.since < kTapMs && !g_chord)
        {
            if (g_b2.down)
            {
                engine.SetRef((engine.Ref() + 1) % 3);
                g_b2_used = true;
            }
            else
            {
                g_sel   = (uint8_t)((g_sel + 1) % tether::kVoices);
                g_rearm = true;
                mark_dirty(now);
            }
        }
        g_b1.down = false;
    }

    /* B2: tap = all notes off, hold = MASTER (letting go ends the reference). */
    if (b2 && !g_b2.down) { g_b2.down = true; g_b2.since = now; g_b2_used = false; }
    if (!b2 && g_b2.down)
    {
        if (now - g_b2.since < kTapMs && !g_chord && !g_b2_used) g_cmd.Push(kCmdPanic);
        engine.SetRef(0);
        g_b2.down = false;
    }

    /* B3: tap = a test note on the voice, hold = JACKS. */
    if (b3 && !g_b3.down) { g_b3.down = true; g_b3.since = now; g_b3_used = false; }
    if (!b3 && g_b3.down)
    {
        if (now - g_b3.since < kTapMs && !g_chord && !g_b3_used && g_test_voice < 0)
        {
            g_test_voice = g_sel;
            g_test_until = now + kTestMs;
            g_cmd.Push(kCmdTestOn | (uint32_t)g_sel << 8);
        }
        g_b3.down = false;
    }

    if (!b1 && !b2 && !b3) g_chord = false;

    Layer want = kLayerVoice;
    if (!g_chord)
    {
        if (b3 && (now - g_b3.since >= kTapMs || g_b3_used)) want = kLayerJacks;
        else if (b2 && (now - g_b2.since >= kTapMs || g_b2_used)) want = kLayerMaster;
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
        if (fabsf(x - c.last) < 0.004f) continue;
        c.last = x;
        if (apply(g_layer, p, x))
        {
            mark_dirty(now);
            if (g_layer == kLayerJacks) g_b3_used = true;
        }
    }

    if (g_dirty && now - g_dirty_since > kAutosaveMs && !g_b1.down && !g_b2.down && !g_b3.down
        && !picker::Busy())
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

static int arc_index(float norm) { return (int)lroundf(norm * (float)(hw.Arc().arc_leds - 1)); }

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

static void OnRender(uint32_t t_ms)
{
    if (settings.IsActive()) return;
    LedPanel& L = hw.leds;

    for (uint8_t p = 0; p < kNumPots; p++)
    {
        const PotView v = view(g_layer, p);
        draw_view(p, v);
        if (v.kind != Kind::None && !g_catch[p].caught) ring_led(p, arc_index(g_phys[p]), kCatchCol);
    }
    /* JACKS: each ring lights its last LED while its jack is high. */
    if (g_layer == kLayerJacks)
        for (uint8_t p = 0; p < kNumPots; p++)
            if (engine.JackVolts(p) > 2.5f) ring_led(p, hw.Arc().arc_leds - 1, kCalCol);

    /* B1: the voice's colour, bright while its gate is up; white blinks
     * while a calibration reference is out. */
    if (engine.Ref())
        L.SetButtonPair(kButtonB1, L.ScaleGlobal((t_ms / 250u) % 2u ? kCalCol : scale(kCalCol, 0.1f)));
    else
        L.SetButtonPair(kButtonB1, L.ScaleGlobal(scale(kVoiceCol[g_sel],
                                                       engine.Gate(g_sel) ? 1.0f : 0.15f)));

    /* B2: the MIDI clock's beat (white), dim amber without a clock. */
    {
        const bool beat = engine.ClockSeen() && engine.BeatPhase() < 0.15f;
        L.SetButtonPair(kButtonB2, L.ScaleGlobal(engine.ClockSeen() ? scale(kCalCol, beat ? 1.0f : 0.08f)
                                                                    : scale(kMasterCol, 0.1f)));
    }

    /* B3: the USB host. Blue: Mac mode. Red dim: nothing plugged in;
     * amber: starting a device; red: not one it can use; green: MIDI,
     * flashing white with each message. */
    Rgb b3;
    if (g_mac_mode) b3 = kBlue;
    else switch (g_stage)
    {
        case 3: b3 = engine.MidiFlash() > 0.5f ? kCalCol : kGreen; break;
        case 2: b3 = kRed; break;
        case 1: b3 = kAmber; break;
        default: b3 = scale(kRed, 0.15f); break;
    }
    L.SetButtonPair(kButtonB3, L.ScaleGlobal(b3));
}

/* ---- audio ----------------------------------------------------------------- */

static void AudioCallback(daisy::AudioHandle::InputBuffer, daisy::AudioHandle::OutputBuffer out, size_t n)
{
    uint32_t c;
    while (g_cmd.Pop(&c))
    {
        const int v = (int)(c >> 8);
        switch (c & 0xFF)
        {
            case kCmdTestOn: engine.Test(v, true); break;
            case kCmdTestOff: engine.Test(v, false); break;
            case kCmdPanic: engine.Panic(); break;
        }
    }
    uint32_t pkt;
    while (midihost::Pop(&pkt)) engine.Packet(pkt);
    engine.Process((uint32_t)n);

    for (size_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;
    for (int j = 0; j < 4; j++) g_mcp_volts[j] = engine.JackVolts(j);   /* J3-J6 */
    hw.cv_jacks[4].SetVolts(engine.JackVolts(4));                        /* J7 */
    hw.cv_jacks[5].SetVolts(engine.JackVolts(5));                        /* J8 */
    hw.cv_jacks[6].SetVolts(engine.PitchVolts(0));                       /* J9: pitch A */
    hw.cv_jacks[7].SetVolts(engine.PitchVolts(1));                       /* J10: pitch B */
}

/* ---- main ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    engine.Init(hw.SampleRate());

    for (uint8_t j = 0; j < kNumCvJacks; j++)
    {
        hw.cv_jacks[j].SetVolts(0.0f);
        hw.cv_jacks[j].EnableCvOutput();
    }

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);

    host.Product("TETHER")
        .BootSlot(kHomeSlot)
        .Jacks(jk_j1, jk_j2, jk_j3, jk_j4, jk_j5, jk_j6, jk_j7, jk_j8, jk_j9, jk_j10)
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

    /* B1 held at power-up: Mac mode, the port stays a USB device so the Mac
     * (or the iPhone app) can reach this firmware and load another. The
     * shared /alchemy/usb.cfg is never written. */
    g_mac_mode = hw.buttons[kButtonB1].Pressed();
    if (g_mac_mode) g_b1 = Btn{true, 0};   /* letting go of it is not a tap */
    g_boot_ms  = System::GetNow();

    hw.StartAudio(AudioCallback);

    loop.Use(settings).OnFrame(OnFrame).OnPoll(OnPoll).OnRender(OnRender);
    if (g_mac_mode) loop.Use(host);

    for (;;) loop.Tick();
}
