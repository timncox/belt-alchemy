/*
 * Belt on the Hermetic Modular Alchemy Lab V2 -- live vocal processor as
 * Alchemy SDK firmware: pitch correction, four diatonic harmony voices, a
 * doubler and a formant control on a Eurorack insert.
 *
 * The DSP is belt_core, vendored from schwung-belt (see vendor/belt_core.h
 * for the one edit: 48 kHz). This file is the host shim: six pots on two
 * pages, three buttons, six CV jacks and 102 LEDs to the engine's string
 * parameter API.
 *
 * Shared by copy with smack-alchemy and mark-alchemy: the shape of this
 * file, the picker, the allocator, the autosave. See DESIGN.md.
 *
 *   PLAY   KEY · SCALE · RETUNE · AMOUNT · HARMONY · FORMANT
 *   SETUP  VOICE 1-4 intervals · DOUBLER · SPREAD          (hold B3)
 *   B1     HARD tune: hold for momentary, tap to latch
 *   B2     harmonies: hold to mute, tap to latch the mute
 *   J3     HARD gate; J4-J8 CV to KEY / RETUNE / AMOUNT / HARMONY / FORMANT
 *   Settings (B2+B3 2 s): FLEX, HUMANIZE, WET; page 1 = the SD firmware picker
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daisy_seed.h"
#include "hid/usb.h"
#include "util/CpuLoadMeter.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/hw/v2_calibration.h"
#include "alchemy/host_link/cdc_transport.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "extras.h"
#include "picker.h"
#include "versio_alloc.h"

/* versio_alloc.h first, then the engine inside extern "C" -- see the note in
 * versio_alloc.h about include order and linkage. */
extern "C" {
#include "vendor/belt_core.h"
}

#ifndef BELT_VERSION
#define BELT_VERSION "0.0.0"
#endif
#ifndef BELT_GIT_HASH
#define BELT_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* The engine takes any block; 128 keeps the callback overhead where the
 * family's other ports measured it. Its own latency is fixed at
 * BELT_LATENCY (1152 frames, 24 ms) regardless. */
static constexpr uint32_t kBlockSize = 128u;

/* J3 is the HARD gate: CV index 0. */
static constexpr uint8_t kGateJack = 0u;

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* Sized in versio_alloc.h so this and the native test cannot drift. */
static uint8_t DSY_SDRAM_BSS g_pool[VERSIO_POOL_BYTES];

static belt_t*       B = nullptr;
static host_api_v1_t HOST;

/* Engine readbacks published by the control loop for the LEDs. */
static int  G_NOTE10 = 0;      /* detected MIDI note x10 */
static int  G_CENTS  = 0;      /* cents from the correction target */
static int  G_VOICED = 0;
static int  G_MASK   = 0;      /* harmony voices with an interval set */
static bool G_HARD   = false;  /* effective hard-tune state */
static bool G_MUTED  = false;  /* harmonies muted */

/* ---- colours ------------------------------------------------------------ */

static constexpr LedPanel::Rgb kColKey     = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColScale   = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColInScale = {0x30, 0x1C, 0x08};
static constexpr LedPanel::Rgb kColRetune  = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColAmount  = {0x60, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColHarm    = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColFmtUp   = {0xFF, 0x60, 0xA0};
static constexpr LedPanel::Rgb kColFmtDown = {0x60, 0xA0, 0xFF};
static constexpr LedPanel::Rgb kColNotch   = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColV1      = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColV2      = {0xFF, 0x60, 0xC0};
static constexpr LedPanel::Rgb kColV3      = {0x60, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColV4      = {0x60, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColDouble  = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColSpread  = {0x80, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim     = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup   = {0x30, 0x10, 0x40};

static constexpr LedPanel::Rgb kIdle   = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kAmber  = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kRed    = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kGreen  = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kWhite  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kGrey   = {0x60, 0x60, 0x60};
static constexpr LedPanel::Rgb kPurple = {0x80, 0x00, 0xC0};

/* ---- labels (descriptor metadata; static storage, borrowed by pointer) -- */

static const char* const kKeyLabels[12] = {"C", "C#", "D", "D#", "E", "F",
                                            "F#", "G", "G#", "A", "A#", "B"};
/* Engine order (belt_core.c scale_deg). */
static const char* const kScaleLabels[9] = {"Chromatic", "Major", "Minor", "Harm Minor",
                                             "Dorian", "Mixolydian", "Maj Pent",
                                             "Min Pent", "Blues"};
/* Engine order (belt_core.c interval enum; 6 = unison). */
static const char* const kItvLabels[12] = {"Off", "-Oct", "-6th", "-5th", "-4th", "-3rd",
                                            "Unison", "+3rd", "+4th", "+5th", "+6th", "+Oct"};

/* The engine's scale tables, for the KEY ring's in-scale dots. Duplicated
 * rather than exported: the engine's are file-static, and this is display
 * only -- the engine decides what is in the scale. Keep in step with
 * belt_core.c scale_deg / scale_len. */
static const int kScaleLen[9]     = {12, 7, 7, 7, 7, 7, 5, 5, 6};
static const int kScaleDeg[9][12] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {0, 2, 4, 5, 7, 9, 11},
    {0, 2, 3, 5, 7, 8, 10},
    {0, 2, 3, 5, 7, 8, 11},
    {0, 2, 3, 5, 7, 9, 10},
    {0, 2, 4, 5, 7, 9, 10},
    {0, 2, 4, 7, 9},
    {0, 3, 5, 7, 10},
    {0, 3, 5, 6, 7, 10},
};

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]                                                        */

static VirtualKnob key = VirtualKnob(kPotTopLeft, "Key")
    .Selector(12).Labels(kKeyLabels).Ident("key")
    .Ring(SelectorRing(kColKey, kColDim, 12));

static VirtualKnob scale = VirtualKnob(kPotTopRight, "Scale")
    .Selector(9).Labels(kScaleLabels).Ident("scale")
    .Ring(SelectorRing(kColScale, kColDim, 9));

static VirtualKnob retune = VirtualKnob(kPotMiddleLeft, "Retune")
    .Linear(0.f, 100.f).Ident("retune").Unit("%")
    .Ring(Level(kColRetune));

static VirtualKnob amount = VirtualKnob(kPotMiddleRight, "Amount")
    .Linear(0.f, 100.f).Ident("amount").Unit("%")
    .Ring(Level(kColAmount));

static VirtualKnob harmony = VirtualKnob(kPotBottomLeft, "Harmony")
    .Linear(0.f, 100.f).Ident("harm_level").Unit("%")
    .Ring(Level(kColHarm));

static VirtualKnob formant = VirtualKnob(kPotBottomRight, "Formant")
    .Linear(-100.f, 100.f).Ident("formant")
    .Ring(Bipolar(kColFmtUp, kColFmtDown, kColNotch));

/* SETUP: hold B3. The four harmony voices and the doubler. */

static VirtualKnob harm1 = VirtualKnob(kPotTopLeft, "Voice 1")
    .Selector(12).Labels(kItvLabels).Ident("harm1")
    .Ring(SelectorRing(kColV1, kColDim, 12));

static VirtualKnob harm2 = VirtualKnob(kPotTopRight, "Voice 2")
    .Selector(12).Labels(kItvLabels).Ident("harm2")
    .Ring(SelectorRing(kColV2, kColDim, 12));

static VirtualKnob harm3 = VirtualKnob(kPotMiddleLeft, "Voice 3")
    .Selector(12).Labels(kItvLabels).Ident("harm3")
    .Ring(SelectorRing(kColV3, kColDim, 12));

static VirtualKnob harm4 = VirtualKnob(kPotMiddleRight, "Voice 4")
    .Selector(12).Labels(kItvLabels).Ident("harm4")
    .Ring(SelectorRing(kColV4, kColDim, 12));

static VirtualKnob doubler = VirtualKnob(kPotBottomLeft, "Doubler")
    .Linear(0.f, 100.f).Ident("double_amt").Unit("%")
    .Ring(Level(kColDouble));

static VirtualKnob spread = VirtualKnob(kPotBottomRight, "Spread")
    .Linear(0.f, 100.f).Ident("spread").Unit("%")
    .Ring(Level(kColSpread));

static Page play_page = Page(kPagePlay).Name("Play").Color("#ff9020")
    .Knobs(key, scale, retune, amount, harmony, formant);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#c060ff")
    .Knobs(harm1, harm2, harm3, harm4, doubler, spread);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_in_l   ("J1",  "In L",       JackSig::AudioIn);
static Jack jk_in_r   ("J2",  "In R",       JackSig::AudioIn);
static Jack jk_gate   ("J3",  "Hard",       JackSig::Trig);
static Jack jk_cv_key ("J4",  "CV Key",     JackSig::CvBi);
static Jack jk_cv_ret ("J5",  "CV Retune",  JackSig::CvBi);
static Jack jk_cv_amt ("J6",  "CV Amount",  JackSig::CvBi);
static Jack jk_cv_hrm ("J7",  "CV Harmony", JackSig::CvBi);
static Jack jk_cv_fmt ("J8",  "CV Formant", JackSig::CvBi);
static Jack jk_out_l  ("J9",  "Out L",      JackSig::AudioOut);
static Jack jk_out_r  ("J10", "Out R",      JackSig::AudioOut);

static VirtualButton bt_hard = VirtualButton("b1", "Hard")
    .Action("Hold", "Hard-tune while held: instant, full correction")
    .Action("Tap", "Latch hard-tune on / off");

static VirtualButton bt_harm = VirtualButton("b2", "Harmonies")
    .Action("Hold", "Mute the harmony voices while held")
    .Action("Tap", "Latch the mute on / off");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Live vocal processor: correction, four harmonies, doubler, formant")
    .Preamble(
        "**Belt** listens to the voice on J1/J2, corrects it to the KEY and "
        "SCALE at the RETUNE speed (0 is the hard-tune robot), and adds up to "
        "four harmony voices that walk the scale from what you sing. Set the "
        "voices' intervals on the Setup page (hold B3), with the doubler and "
        "the stereo spread. Hold B1 for a hard-tune punch, tap it to latch; "
        "B2 mutes the harmonies the same way. The KEY ring is a tuner: the "
        "note you are singing lights green when it is within a quarter tone "
        "of a scale note, orange when it is not. A gate on J3 punches hard-"
        "tune from a sequencer.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/*
 * This firmware's own preset slot. Every Alchemy firmware in the family
 * keeps its working state in a slot of its own -- Smack 12, Mark 13, Belt
 * 14, Relay 15 -- so switching firmware through the SD picker no longer
 * resets the one you left. Slot 0, where they all used to autosave, is
 * read once at boot for migration only (see main). Slots 0-11 are left for
 * presets saved by hand; saving one by hand into 12-15 overwrites that
 * firmware's working state.
 */
static constexpr uint8_t kHomeSlot = 14u;
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, &pager);
static CvMatrix    cv_matrix(kNumCvInputs);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "belt_alchemy", "Belt",
                           BELT_VERSION, BELT_GIT_HASH);
static BeltExtras  extras;
static KnobHandle  flex_k, humanize_k, wet_k;

#ifdef BELT_BENCH_USB
/* HostLink on the Seed's own micro-USB instead of the front panel, for the
 * bench-bootloader setup where the front port is not serving anything. */
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- host shim ------------------------------------------------------------ */

static float host_get_bpm(void) { return 120.0f; }   /* the engine never asks */

/* ---- parameter dispatch ------------------------------------------------- */

/*
 * The engine takes strings, so a parameter is only sent when its quantized
 * value actually changes. Stepped ones also need hysteresis: a pot parked on
 * a step boundary would otherwise chatter across it on ADC noise, and with
 * CV summed in a KEY that chatters re-targets every harmony. The SDK leaves
 * zone-commit debouncing to the app (docs/pages-and-layers.md).
 */
#define HYST 0.02f /* ~2% of travel */

struct Knob
{
    const char* key;      /* engine parameter */
    int         lo, hi;   /* inclusive integer range */
    bool        zones;    /* floor(norm * N), matching the SDK's selector
                             rings, else nearest */
    float       dead;     /* deadband in normalised units; 0 = none */
    int         last      = -32768;   /* -32768 = never dispatched */
    float       last_norm = 0.0f;
};

static Knob k_key     {"key",        0,   11, true,  HYST};
static Knob k_scale   {"scale",      0,    8, true,  HYST};
static Knob k_retune  {"retune",     0,  100, false, 0.0f};
static Knob k_amount  {"amount",     0,  100, false, 0.0f};
static Knob k_harm    {"harm_level", 0,  100, false, 0.0f};
static Knob k_formant {"formant", -100,  100, false, 0.0f};
static Knob k_h1      {"harm1",      0,   11, true,  HYST};
static Knob k_h2      {"harm2",      0,   11, true,  HYST};
static Knob k_h3      {"harm3",      0,   11, true,  HYST};
static Knob k_h4      {"harm4",      0,   11, true,  HYST};
static Knob k_double  {"double_amt", 0,  100, false, 0.0f};
static Knob k_spread  {"spread",     0,  100, false, 0.0f};

static int knob_quantize(const Knob& k, float norm)
{
    const int span = k.hi - k.lo;
    int       v;
    if (k.zones)
    {
        v = (int)(norm * (float)(span + 1));
        if (v > span) v = span;
    }
    else
    {
        v = (int)(norm * (float)span + 0.5f);
    }
    if (v < 0)    v = 0;
    if (v > span) v = span;
    return k.lo + v;
}

/* True, with *out set, when the quantized value moved far enough to count. */
static bool knob_changed(Knob& k, float norm, int* out)
{
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    const int v = knob_quantize(k, norm);
    if (v == k.last) return false;
    if (k.dead > 0.0f && k.last != -32768)
    {
        const float moved = fabsf(norm - k.last_norm);
        if (moved < k.dead) return false;
    }
    k.last      = v;
    k.last_norm = norm;
    *out        = v;
    return true;
}

static void set_param_int(const char* key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    belt_set_param(B, key, b);
}

static bool dispatch(Knob& k, float norm)
{
    int v;
    if (!knob_changed(k, norm, &v)) return false;
    set_param_int(k.key, v);
    return true;
}

/* ---- HARD gate on J3 ------------------------------------------------------ */

/*
 * J3 is read as raw ADC in the audio callback, not through the SDK's
 * AnalogControl: that path is low-passed at ~20 Hz for pots, which would
 * round a gate's edges into a swell. The board's calibration record gives
 * the jack's 0 V code and the ADC reference, so the thresholds are in volts.
 * The deviation MAGNITUDE is thresholded, so the gate reads whichever way
 * the ADC counts -- the sign of the front end is documented (higher code =
 * more negative jack volts) but not verified here.
 */
static uint16_t      g_gate_zero  = 32768u;
static int           g_gate_hi    = 4915;   /* +1.5 V at VDDA 3.30: assert  */
static int           g_gate_lo    = 1638;   /* +0.5 V:               release */
static volatile bool g_gate_state = false;

static void gate_calibrate(void)
{
    const V2Calibration& cal = hw.Calibration();
    float vdda = cal.vdda_at_cal;
    if (!(vdda > 2.5f && vdda < 3.6f)) vdda = kV2VddaDesign;
    uint16_t zero = cal.jack[kGateJack].adc_zero_code;
    if (zero < 20000u || zero > 45000u) zero = 32768u;   /* design mid-scale */
    const float cpv = kV2CvInGainDesign * 65535.0f / vdda;  /* ADC counts per volt */
    g_gate_zero = zero;
    g_gate_hi   = (int)(1.5f * cpv);
    g_gate_lo   = (int)(0.5f * cpv);
}

static inline void gate_poll_isr(void)
{
    const uint16_t raw = hw.seed.adc.Get(kCvAdcOffset + kGateJack);
    int            dev = (int)g_gate_zero - (int)raw;
    if (dev < 0) dev = -dev;
    if (!g_gate_state) { if (dev > g_gate_hi) g_gate_state = true; }
    else if (dev < g_gate_lo) g_gate_state = false;
}

/* ---- buttons (1 ms poll) -------------------------------------------------- */

/*
 * B1 HARD and B2 HARMONIES share one gesture: the button is momentary while
 * held, and a tap (released within TAP_MS) toggles a latch instead. The
 * effective state is latch XOR held, so a hold from a latched state gives
 * the opposite for the duration and comes back on release. J3 ORs into HARD.
 *
 * Edges come from Pressed(), not RisingEdge(): the ButtonBank is not in use
 * and the framework's poll consumes nothing, but a 1 ms poll against a
 * debouncer's own edge flag is a coin flip (see smack-versio's note).
 */
#define TAP_MS 300u

struct Toggle
{
    bool     down  = false;
    bool     latch = false;
    uint32_t t0    = 0;

    /* Returns the effective state after this poll. */
    bool Poll(bool pressed, uint32_t now)
    {
        if (pressed && !down) { down = true; t0 = now; }
        if (!pressed && down)
        {
            down = false;
            if (now - t0 < TAP_MS) latch = !latch;
        }
        return latch != down;
    }
    void Reset() { down = false; }
};

static Toggle tg_hard, tg_mute;

static void apply_hard(bool on)
{
    if (on == G_HARD) return;
    G_HARD = on;
    set_param_int("hard", on ? 1 : 0);
}

static void apply_mute(bool on)
{
    if (on == G_MUTED) return;
    G_MUTED = on;
    /* Muted: the engine sees no harmony level. Unmuted: the pot's value
     * again, whatever it was last quantized to. */
    set_param_int("harm_level", on ? 0 : (k_harm.last < 0 ? 0 : k_harm.last));
}

static void OnPoll(uint32_t now)
{
    if (settings.IsActive())
    {
        /* Settings owns the buttons; drop momentaries, keep latches. */
        tg_hard.Reset();
        tg_mute.Reset();
        apply_hard(tg_hard.latch || g_gate_state);
        apply_mute(tg_mute.latch);
        return;
    }
    const bool hard = tg_hard.Poll(hw.buttons[kButtonB1].Pressed(), now) || g_gate_state;
    /* B2 stands down while B3 is held: B2+B3 held two seconds is the
     * Settings chord, and B2's own gesture would mute the harmonies on the
     * way in and, if B3 landed inside the tap window, flip the latch. */
    const bool b3   = hw.buttons[kButtonB3].Pressed();
    if (b3) tg_mute.Reset();
    const bool mute = b3 ? tg_mute.latch
                         : tg_mute.Poll(hw.buttons[kButtonB2].Pressed(), now);
    apply_hard(hard);
    apply_mute(mute);
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static bool     g_dirty       = false;
static uint32_t g_dirty_since = 0;
static float    g_saved_peak  = 0.0f;
static bool     prev_settings = false;
static int      applied_flex = -1, applied_humanize = -1, applied_wet = -1;

static void mark_dirty(uint32_t now)
{
    g_dirty       = true;
    g_dirty_since = now;
}

static int settings_pct(const KnobHandle& k)
{
    float v = k.Value();
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (int)lroundf(v * 100.0f);
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();
    int            v;

    /* The PLAY page is worth a flash write too. It used to follow the pots
     * at every boot, so saving it bought nothing; now that a saved PLAY page
     * is restored (the pots catch it), a change there must reach the slot or
     * it is lost at the next firmware switch or power cycle. The stored
     * values move only when a caught pot moves, and the 1 % step keeps ADC
     * noise from re-arming the autosave. The first frame only primes. */
    {
        static float play_seen[kNumPots];
        static bool  primed = false;
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            const float s = pager.Stored(kPagePlay, p);
            if (!primed) { play_seen[p] = s; continue; }
            if (fabsf(s - play_seen[p]) > 0.01f)
            {
                play_seen[p] = s;
                mark_dirty(now);
            }
        }
        primed = true;
    }

    /* PLAY page. Values are catch + locks + CV, already mixed by the SDK. */
    dispatch(k_key,     key.Norm());
    dispatch(k_scale,   scale.Norm());
    dispatch(k_retune,  retune.Norm());
    dispatch(k_amount,  amount.Norm());
    dispatch(k_formant, formant.Norm());
    if (knob_changed(k_harm, harmony.Norm(), &v) && !G_MUTED)
        set_param_int("harm_level", v);

    /* SETUP page. These are worth a flash write when they move. */
    if (dispatch(k_h1,     harm1.Norm()))   mark_dirty(now);
    if (dispatch(k_h2,     harm2.Norm()))   mark_dirty(now);
    if (dispatch(k_h3,     harm3.Norm()))   mark_dirty(now);
    if (dispatch(k_h4,     harm4.Norm()))   mark_dirty(now);
    if (dispatch(k_double, doubler.Norm())) mark_dirty(now);
    if (dispatch(k_spread, spread.Norm()))  mark_dirty(now);

    /* Settings knobs: the three the panel has no room for. */
    {
        const int f = settings_pct(flex_k), h = settings_pct(humanize_k), w = settings_pct(wet_k);
        if (f != applied_flex)     { applied_flex     = f; set_param_int("flex",     f); }
        if (h != applied_humanize) { applied_humanize = h; set_param_int("humanize", h); }
        if (w != applied_wet)      { applied_wet      = w; set_param_int("wet",      w); }
    }

    /* Engine readback, one snprintf per frame: "note10:cents:voiced:mask". */
    {
        char buf[48];
        int  n10 = 0, cents = 0, voiced = 0, mask = 0;
        if (belt_get_param(B, "status", buf, sizeof buf) > 0
            && sscanf(buf, "%d:%d:%d:%d", &n10, &cents, &voiced, &mask) == 4)
        {
            G_NOTE10 = n10;
            G_CENTS  = cents;
            G_VOICED = voiced;
            G_MASK   = mask;
        }
    }

    /* Worst block this session. Clamped: an overrunning callback can report
     * over 100%, and the readout wants 0..1. */
    {
        float mx = cpu.GetMaxCpuLoad();
        if (mx > 1.0f) mx = 1.0f;
        if (mx > extras.cpu_peak) extras.cpu_peak = mx;
        if (extras.cpu_peak > g_saved_peak + 0.05f) mark_dirty(now);
    }

    /* A visit to Settings is worth saving (brightness, flex, humanize, wet). */
    const bool sact = settings.IsActive();
    if (prev_settings && !sact) mark_dirty(now);
    prev_settings = sact;

    /*
     * Autosave slot 0, the working state, a few seconds after the last
     * change and only with hands off: Save() erases the sectors the record
     * covers (one, for this payload) from this thread, and the 1 ms button
     * poll is paused for the duration. The epsilons above are the wear
     * limiter; this is a ceiling on write frequency, not a write rate.
     */
    if (g_dirty && now - g_dirty_since >= 5000u && !sact && !picker::Busy()
        && !hw.buttons[kButtonB1].Pressed() && !hw.buttons[kButtonB2].Pressed()
        && !hw.buttons[kButtonB3].Pressed())
    {
        presets.Save(kHomeSlot);
        g_dirty      = false;
        g_saved_peak = extras.cpu_peak;
    }
}

/* ---- LEDs ------------------------------------------------------------------ */

/*
 * The SDK draws the rings from the knobs. What is added:
 *   KEY ring         the tuner: in-scale notes as dim dots on the SDK's
 *                    12-zone selector, the sung note as a green pip within
 *                    a quarter tone of a scale note, orange outside it
 *   B1 pair          white while hard-tune is on; else the tuner colour
 *                    (green in tune, orange off, dim blue when unvoiced)
 *   B2 pair          purple while harmony voices are active, grey muted,
 *                    dim when no voice has an interval; red when the
 *                    audio callback is above 80%
 *   B3 pair          dim Setup tint (the SDK paints it while the page is held)
 *   P1 ring at boot  the previous session's worst CPU load, for 2.5 s
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static float zone_hour(const ArcGeometry& geo, int n)
{
    /* The SDK's Distributed selector: zone i of 12 sits at LED round(i*12/11). */
    const int led = (int)lroundf((float)n * (float)(geo.arc_leds - 1) / 11.0f);
    return fmodf(geo.start_hour + geo.step_hours * (float)led, 12.0f);
}

static bool in_scale(int n, int key_idx, int scale_idx)
{
    const int pc = ((n - key_idx) % 12 + 12) % 12;
    for (int i = 0; i < kScaleLen[scale_idx]; i++)
        if (kScaleDeg[scale_idx][i] == pc) return true;
    return false;
}

static void KeyOverdraw(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                        float norm, uint32_t t_ms, void* ctx)
{
    (void)norm; (void)t_ms; (void)ctx;
    const int key_idx   = k_key.last   < 0 ? 0 : k_key.last;
    const int scale_idx = k_scale.last < 0 ? 1 : k_scale.last;

    for (int n = 0; n < 12; n++)
        if (n != key_idx && in_scale(n, key_idx, scale_idx))
            panel.SetRingByHour(pot, zone_hour(geo, n), panel.ScaleGlobal(kColInScale));

    if (G_VOICED)
    {
        const int n = ((G_NOTE10 + 5) / 10 % 12 + 12) % 12;
        const LedPanel::Rgb c = (G_CENTS <= 25 && G_CENTS >= -25) ? kGreen : kAmber;
        panel.SetRingByHour(pot, zone_hour(geo, n), panel.ScaleGlobal(c));
    }
}

static void paint_fill(uint8_t pot, float frac, const LedPanel::Rgb& c)
{
    const ArcGeometry& geo = hw.Arc();
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int n = (int)lroundf(frac * (float)geo.arc_leds);
    for (int i = 0; i < n; i++)
        hw.leds.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)i, 12.0f),
                              hw.leds.ScaleGlobal(c));
}

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;

    if (t_ms < g_readout_until)
    {
        L.ClearRing(kPotTopLeft);
        if (g_boot_peak <= 0.0f)
        {
            /* No data: first boot after a flash. Distinct from "measured,
             * and low" so the two never get confused. */
            L.SetRingByHour(kPotTopLeft, hw.Arc().start_hour, L.ScaleGlobal(kIdle));
        }
        else
        {
            const LedPanel::Rgb c = g_boot_peak >= 0.90f ? kRed
                                  : g_boot_peak >= 0.75f ? kAmber
                                                         : kGreen;
            paint_fill(kPotTopLeft, g_boot_peak, c);
        }
    }

    if (settings.IsActive()) return;   /* Settings owns the buttons */

    LedPanel::Rgb b1;
    if (G_HARD)        b1 = kWhite;
    else if (!G_VOICED) b1 = kIdle;
    else               b1 = (G_CENTS <= 25 && G_CENTS >= -25) ? kGreen : kAmber;
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(b1));

    LedPanel::Rgb b2;
    if (cpu.GetAvgCpuLoad() > 0.80f) b2 = kRed;
    else if (G_MUTED)                b2 = kGrey;
    else if (G_MASK)                 b2 = kPurple;
    else                             b2 = kIdle;
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2));

    if (pager.Page() != kPageSetup)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColSetup));
}

/* ---- audio ----------------------------------------------------------------- */

static int16_t bufi[kBlockSize * 2];

static inline int16_t f2i(float v)
{
    if (v > 0.999969f) v = 0.999969f;
    if (v < -1.0f)     v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    /* Controls are NOT read here; only the gate needs an edge this fast. */
    gate_poll_isr();

    /* Refuse to run rather than corrupt memory if the block is ever not what
     * Init() asked for. */
    if (size > kBlockSize)
    {
        for (size_t i = 0; i < size; i++)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        cpu.OnBlockEnd();
        return;
    }

    /* float -1..1 -> interleaved int16, which is what the engine takes.
     * Converting at the boundary keeps the engine bit-identical to the Move
     * build, so any difference in sound is a shim bug. The engine sums the
     * two inputs to mono for analysis and keeps the dry path stereo. */
    for (size_t i = 0; i < size; i++)
    {
        bufi[2 * i]     = f2i(in[0][i]);
        bufi[2 * i + 1] = f2i(in[1][i]);
    }

    belt_process(B, bufi, bufi, (int)size);

    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = (float)bufi[2 * i] * k;
        out[1][i] = (float)bufi[2 * i + 1] * k;
    }

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

static void fault_forever(void)
{
    /* Refuse to run half-initialised: every button pair red, no audio. */
    for (;;)
    {
        hw.leds.Clear();
        for (uint8_t b = 0; b < kNumButtons; b++)
            hw.leds.SetButtonPair(b, {0x80, 0x00, 0x00});
        hw.leds.Show();
        System::Delay(200);
    }
}

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    versio_alloc_init(g_pool, VERSIO_POOL_BYTES);
    gate_calibrate();

    memset(&HOST, 0, sizeof HOST);
    HOST.api_version      = 1;
    HOST.sample_rate      = BELT_SR;
    HOST.frames_per_block = kBlockSize;
    HOST.get_bpm          = host_get_bpm;

    B = belt_create(&HOST);
    if (!B || versio_alloc_failed()) fault_forever();

    /* The output is always live on a Eurorack insert; the Move's feedback
     * guard has no microphone here to protect. */
    belt_set_param(B, "monitor",  "1");
    belt_set_param(B, "hw_input", "1");

    /* Storage + settings. sd.Init() registers the SDRAM volume; no card I/O
     * until something opens it. */
    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    flex_k = settings.Page(kSettingsMain).Pot(1).Knob().Default(0.0f)
        .Ident("flex").Name("Flex").Color(kColRetune)
        .Help("How far from any scale note the singing may stray before "
              "correction lets go of it. 0 corrects everything.");
    humanize_k = settings.Page(kSettingsMain).Pot(4).Knob().Default(0.3f)
        .Ident("humanize").Name("Humanize").Color(kColAmount)
        .Help("Vibrato preserved through the correction, and a slow wander "
              "on each harmony voice so they do not sound like one singer.");
    wet_k = settings.Page(kSettingsMain).Pot(5).Knob().Default(1.0f)
        .Ident("wet").Name("Wet").Color(kColKey)
        .Help("Corrected lead against the dry voice. Harmonies and the "
              "doubler add on top either way.");

    /* CV: J3 is the HARD gate (read raw in the callback), the rest modulate. */
    cv_matrix.Jack(0).Off();
    cv_matrix.Jack(1).To(key);
    cv_matrix.Jack(2).To(retune);
    cv_matrix.Jack(3).To(amount);
    cv_matrix.Jack(4).To(harmony);
    cv_matrix.Jack(5).To(formant);

    key.Overdraw(KeyOverdraw);

    /* HostLink: identity, descriptor, SD file access. The transport is the
     * front USB-C unless this is a bench build. Must all be declared before
     * BootLoad(), which is where the host starts. */
    host.Product("Belt")
        .BootSlot(kHomeSlot)
        .Pages(play_page, setup_page)
        .Jacks(jk_in_l, jk_in_r, jk_gate, jk_cv_key, jk_cv_ret, jk_cv_amt,
               jk_cv_hrm, jk_cv_fmt, jk_out_l, jk_out_r)
        .Buttons(bt_hard, bt_harm, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef BELT_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Belt (bench)");
    host.Transport(bench_cdc);
#endif

    /* Preset payload: both pages, the settings screen, and the extras. */
    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    /* BootLoad() still runs: it fires HostLink's pre-boot hook, and it
     * restores slot 0 when slot 0 holds THIS firmware's settings (the
     * schema gate refuses any other), which carries a module's settings
     * over from the builds that autosaved there. The home slot, once it
     * holds anything, wins. */
    const bool had_home = presets.HasValid(kHomeSlot);
    const bool had_boot = had_home || presets.HasValid(0);
    presets.BootLoad();
    if (had_home) presets.Load(kHomeSlot);

    /* Physical pot positions, primed. */
    float phys[kNumPots];
    for (int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    if (!had_boot)
    {
        /* First boot after a flash: a third and a fifth above, the other
         * two voices off, no doubler, a wide spread. Zone i of 12 sits at
         * (i + 0.5) / 12. */
        pager.SetStored(kPageSetup, kPotTopLeft,     7.5f / 12.0f, phys); /* +3rd   */
        pager.SetStored(kPageSetup, kPotTopRight,    9.5f / 12.0f, phys); /* +5th   */
        pager.SetStored(kPageSetup, kPotMiddleLeft,  0.5f / 12.0f, phys); /* off    */
        pager.SetStored(kPageSetup, kPotMiddleRight, 0.5f / 12.0f, phys); /* off    */
        pager.SetStored(kPageSetup, kPotBottomLeft,  0.0f,         phys); /* no dbl */
        pager.SetStored(kPageSetup, kPotBottomRight, 0.7f,         phys); /* spread */
    }

    /* The PLAY page. With nothing saved (first boot after a flash) it
     * adopts the pots, so whatever a pot points at is what the module
     * does. With a saved state it keeps the saved values, and each pot
     * must catch its value before it takes over: the pots are shared by
     * every firmware on the card, so after a picker switch they point
     * wherever the last firmware left them, and adopting them would
     * overwrite this one's settings with another's. BootLoad() left the
     * Pager a deferred re-arm for its first Update(), which arms the
     * catch against phys without touching the stored values. */
    if (!had_boot)
        for (uint8_t p = 0; p < kNumPots; p++)
            pager.SetStored(kPagePlay, p, phys[p], phys);

    /* Report last session's peak on the P1 ring, then start recording this
     * one. Audio is already passing through during the readout. */
    g_boot_peak      = extras.cpu_peak;
    extras.cpu_peak  = 0.0f;
    g_saved_peak     = 0.0f;
    g_readout_until  = System::GetNow() + 2500u;

    hw.StartAudio(AudioCallback);
    cpu.Reset();

    loop.Use(pager)
        .Use(settings)
        .Use(cv_matrix)
        .Use(host)
        .Use(play_page)
        .Use(setup_page)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);

    for (;;) loop.Tick();
}
