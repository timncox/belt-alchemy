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
 *   B2     harmonies: tap to latch the mute; hold 0.6 s for HOLD (the
 *          harmony voices stay on their notes -- Freeze or Lock)
 *   J3     HARD gate; J4-J8 CV to KEY / RETUNE / AMOUNT / HARMONY / FORMANT
 *   Settings (B2+B3 2 s): FLEX, HUMANIZE, WET; page 1 = the SD firmware picker
 *   Settings page 2 = CHORD: MIDI NOTES, LEAD (0 = chord only), HOLD
 *          (Freeze / Lock), J8 (Formant CV / Hold gate), VEL SENS;
 *   Launchpad top 3 = KEY / PLAY (PLAY pads are held notes: Hide and Seek)
 *   Settings page 4 = SOURCES: where else the held notes come from --
 *          the internal chord sequencer, CV on J4-J7, or MIDI on the rear
 *          header (see "chord sources" below); J3 as the chord clock
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
#include "launchpad.h"
#include "usb_shared.h"
#include "usb_audio.h"
#include "punch_fx.h"
#include "ff.h"
#include "versio_alloc.h"
#include "chord_src.h"
#include "rear_midi.h"

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
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1, kSettingsChord = 2, kSettingsSources = 3 };

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* Sized in versio_alloc.h so this and the native test cannot drift. */
static uint8_t __attribute__((section(".belt_pool"), aligned(32))) g_pool[VERSIO_POOL_BYTES];

static belt_t*       B = nullptr;
static host_api_v1_t HOST;

/* Engine readbacks published by the control loop for the LEDs. */
static int  G_NOTE10 = 0;      /* detected MIDI note x10 */
static int  G_CENTS  = 0;      /* cents from the correction target */
static int  G_VOICED = 0;
static int  G_MASK   = 0;      /* harmony voices with an interval set */
static bool G_HARD   = false;  /* effective hard-tune state */
static bool G_MUTED  = false;  /* harmonies muted */
static bool G_HOLD   = false;  /* HOLD asked for (latch OR the J8 gate) */
static int  G_HOLD_ST = 0;     /* engine: 0 off, 1 locked, 2 frozen, 3 fading */

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
static constexpr LedPanel::Rgb kHold   = {0x10, 0x60, 0xFF};  /* HOLD on */

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
static Jack jk_gate   ("J3",  "Hard / Chord clock", JackSig::Trig);
static Jack jk_cv_key ("J4",  "CV Key / Chord 1",     JackSig::CvBi);
static Jack jk_cv_ret ("J5",  "CV Retune / Chord 2",  JackSig::CvBi);
static Jack jk_cv_amt ("J6",  "CV Amount / Chord 3",  JackSig::CvBi);
static Jack jk_cv_hrm ("J7",  "CV Harmony / Chord 4", JackSig::CvBi);
static Jack jk_cv_fmt ("J8",  "CV Formant / Hold", JackSig::CvBi);
static Jack jk_out_l  ("J9",  "Out L",      JackSig::AudioOut);
static Jack jk_out_r  ("J10", "Out R",      JackSig::AudioOut);

static VirtualButton bt_hard = VirtualButton("b1", "Hard")
    .Action("Hold", "Hard-tune while held: instant, full correction")
    .Action("Tap", "Latch hard-tune on / off");

static VirtualButton bt_harm = VirtualButton("b2", "Harmonies")
    .Action("Tap", "Latch the harmony mute on / off")
    .Action("Hold", "0.6 s: HOLD on / off -- the harmony voices stay on "
                    "their notes while you keep singing");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Live vocal processor: correction, four harmonies, doubler, formant")
    .Preamble(
        "**Belt** listens to the voice on J1/J2, corrects it to the KEY and "
        "SCALE at the RETUNE speed (0 is the hard-tune robot), and adds up to "
        "four harmony voices that walk the scale from what you sing. Set the "
        "voices' intervals on the Setup page (hold B3), with the doubler and "
        "the stereo spread. Hold B1 for a hard-tune punch, tap it to latch. "
        "Tap B2 to mute the harmonies; hold it 0.6 s for HOLD: the voices "
        "keep the notes they are on while you sing on (Freeze sustains them "
        "as a pad, Lock keeps them singing with you -- Settings, Chord "
        "page). The KEY ring is a tuner: the "
        "note you are singing lights green when it is within a quarter tone "
        "of a scale note, orange when it is not. A gate on J3 punches hard-"
        "tune from a sequencer. Chords for the harmony voices can come from "
        "the Launchpad, Belt's own chord sequencer, CV on J4-J7 or MIDI on "
        "the rear header (Settings, Sources).");

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
static KnobHandle  flex_k, humanize_k, wet_k, lead_k, vel_k;
static SelectorHandle midi_mode_s, hold_mode_s, j8_mode_s;
static const char* const kHoldModeLabels[2] = {"Freeze", "Lock"};
static const char* const kJ8Labels[2]       = {"Formant", "Hold gate"};
static const char* const kMidiModeLabels[3] = {"Off", "Harmony", "Target"};
/* The Sources page and the Chord page's Octave (chord sources, below). */
static KnobHandle     tempo_k;
static SelectorHandle source_s, j3_mode_s, tones_s, cv_cal_s, cv_ins_s, octave_s;
static const char* const kSourceLabels[4] = {"Pads", "Seq", "CV", "MIDI"};
static const char* const kJ3Labels[2]     = {"Hard gate", "Chord clock"};
static const char* const kTonesLabels[2]  = {"Triads", "Sevenths"};
static const char* const kCvCalLabels[3]  = {"Off", "On", "Learn"};
static const char* const kCvInsLabels[4]  = {"J4", "J4-J5", "J4-J6", "J4-J7"};
static const char* const kOctaveLabels[4] = {"C2", "C3", "C4", "C5"};

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
/* Rising edges on J3, counted here because a trigger can come and go
 * between two 1 ms polls; the chord clock consumes them (chords_poll). */
static volatile uint32_t g_gate_edges = 0;

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
    if (!g_gate_state)
    {
        if (dev > g_gate_hi)
        {
            g_gate_state = true;
            g_gate_edges = g_gate_edges + 1u;
        }
    }
    else if (dev < g_gate_lo) g_gate_state = false;
}

/* J3 is the HARD gate unless Settings (Sources) makes it the chord clock;
 * then hard-tune is B1 (and the controllers) only. */
static bool g_j3_clock = false;
static inline bool j3_hard(void) { return g_gate_state && !g_j3_clock; }

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

/* ---- HOLD ------------------------------------------------------------------
 *
 * The harmony voices keep the notes they are on while you keep singing.
 * Toggled by B2 held 0.6 s, the Launchpad's fourth top button, the Launch
 * Control XL's third upper button and the gamepad's L3 (the button that was
 * Stutter 1/2); with J8 set to Hold gate (Settings, Chord page), a gate on J8
 * holds while high. Effective HOLD = the latch OR the gate. Never saved: a
 * power cycle comes up with the voices following again.
 */
#define HOLD_MS 600u
static bool          g_hold_latch = false;
static volatile bool g_hold_gate  = false;

static void apply_hold(bool on)
{
    if (on == G_HOLD) return;
    G_HOLD = on;
    set_param_int("hold", on ? 1 : 0);
}

/* J8 in Hold-gate mode: the matrix calls this every 1 ms with the jack's raw
 * reading, 0.5 = 0 V. Like J3 the MAGNITUDE of the deviation is thresholded
 * (+-1.5 V asserts, +-0.5 V releases), so it reads whichever way the front
 * end counts. */
static void OnHoldGate(float cv, uint32_t t_us, void* ctx)
{
    (void)t_us; (void)ctx;
    float dev = cv - 0.5f;
    if (dev < 0.0f) dev = -dev;
    if (!g_hold_gate) { if (dev > 0.15f) g_hold_gate = true; }
    else if (dev < 0.05f) g_hold_gate = false;
}

/* B2's own gesture: a tap (released before HOLD_MS) latches the mute; held
 * to HOLD_MS it toggles HOLD instead, at that moment, and the release does
 * nothing. B2 no longer mutes while held -- the 0.6 s of silence before a
 * HOLD would drop the very voices being held. The Launchpad and XL MUTE
 * buttons keep the hold-to-mute momentary. */
struct B2Gesture
{
    bool     down  = false;
    bool     fired = false;
    uint32_t t0    = 0;

    void Poll(bool pressed, uint32_t now, bool* mute_latch)
    {
        if (pressed && !down) { down = true; fired = false; t0 = now; }
        if (pressed && down && !fired && now - t0 >= HOLD_MS)
        {
            fired        = true;
            g_hold_latch = !g_hold_latch;
        }
        if (!pressed && down)
        {
            down = false;
            if (!fired) *mute_latch = !*mute_latch;
        }
    }
    void Reset() { down = false; }
};
static B2Gesture b2g;

/* ---- Launchpad Mini MK3 -----------------------------------------------------
 *
 *   rows 1-2   KEY, laid out as a keyboard (sharps above: C# D# . F# G# A#,
 *              naturals below: C D E F G A B). The key's root is bright
 *              blue, in-scale notes dim blue, and the sung note is green
 *              within 25 cents, orange outside -- the KEY ring as pads.
 *              Press a pad to set KEY.
 *   rows 4-5   SCALE (9 pads, the current one bright)
 *   row 7      harmony voices 1-4: lit where an interval is set, bright
 *              while singing
 *   row 8      tuner: where the sung note sits, flat on the left, sharp on
 *              the right, green in the middle four when within 25 cents
 *   top 1      HARD tune, top 2  harmony MUTE: the same gestures as B1/B2
 *              (hold for momentary, tap to latch), sharing their latches
 *   top 4      HOLD on / off: blue while held (see HOLD above)
 *   top 3      KEY / PLAY. The layout above is KEY, the default. PLAY turns
 *              rows 1-6 into a three-octave keyboard -- rows 1-2 C5-B5,
 *              3-4 C4-B4, 5-6 C3-B3, sharps above naturals as in KEY -- and
 *              every pad held is a MIDI note into the engine (belt_on_midi):
 *              with MIDI notes on Harmony the harmony voices sing the held
 *              notes, and with LEAD 0 (Settings, the Chord page) only they
 *              sound -- Hide and Seek. Held pads are green, the key's root
 *              blue, in-scale notes dim blue. Rows 7-8 are as in KEY. Top 3
 *              is dim green in KEY, bright in PLAY; leaving PLAY releases
 *              every held pad.
 *   top 5      CHORDS on / off: the internal chord sequencer's progression
 *              (Settings, Sources: Chords from = Seq). Each COLUMN is one
 *              chord of the progression, left to right, up to eight:
 *                rows 1-7  its scale degree, VII at the top down to I in
 *                          row 7 -- press to set it (a column past the end
 *                          lengthens the progression to it). Cyan, the
 *                          playing chord green.
 *                row 8     its length: press to step 1 -> 2 -> 4 -> 8 bars
 *                          -> 1 (dim amber, amber, orange, red)
 *              side 1      run / stop (green running); stopped, the voices
 *                          let go of the chord
 *              side 2 / 3  one chord fewer / one more
 *              Top 5 is magenta in CHORDS; top 5 again goes back to KEY,
 *              top 3 to PLAY.
 *              Key and scale are the panel's; triads or sevenths and the
 *              tempo are on the Sources page. Saved with the slot.
 *
 * USB port (Settings, the firmware page, P5): Mac or Launchpad, from the
 * next power-up. In Launchpad mode B2 shows the host: blue starting, cyan no
 * device, yellow enumerating (or working behind a hub), red gave up, green
 * running. Hold B1 at power-up to boot in Mac mode whatever the setting says.
 */
static bool in_scale(int n, int key_idx, int scale_idx);
static const char* const kUsbLabels[3] = {"Mac", "Launchpad", "Audio"};
static SelectorHandle usb_port;
static bool     g_lp_mode    = false;
/* true = the front USB-C is a USB audio interface this boot (usb_audio.h):
 * computer -> J9/J10, J1/J2 -> computer, and the engine is bypassed. B2:
 * cyan waiting for a computer, yellow connected, green streaming. */
static bool     g_usb_audio  = false;
static uint8_t  g_lp_stage   = 0;
static uint32_t g_lp_boot_ms = 0;
static bool     g_lp_hard_down = false;
static bool     g_lp_mute_down = false;

/* ---- played notes -> the engine ------------------------------------------
 *
 * belt_on_midi() updates the held-note table that belt_update_targets()
 * reads inside belt_process(), which runs in the audio callback. Calling it
 * from the control loop would race the callback, so pads push their notes
 * here and the callback drains the queue before each block: one producer
 * (the control loop), one consumer (the audio callback), no locks. Every
 * chord source pushes here too (chord sources, below) -- never from an
 * interrupt. 64 holds a chord change (up to 8 messages) plus a burst of
 * rear-header MIDI between two blocks; a full queue drops the message. */
struct NoteMsg { uint8_t b[3]; };
static NoteMsg           g_nq[64];
static volatile uint32_t g_nq_w = 0, g_nq_r = 0;

static void note_push(uint8_t status, uint8_t note, uint8_t vel)
{
    const uint32_t w = g_nq_w;
    if (w - g_nq_r >= 64u) return;
    g_nq[w & 63u] = NoteMsg{{status, note, vel}};
    __asm__ volatile("" ::: "memory");   /* the entry before the index */
    g_nq_w = w + 1u;
}

static void note_drain(belt_t* b)   /* audio callback only */
{
    uint32_t r = g_nq_r;
    const uint32_t w = g_nq_w;
    __asm__ volatile("" ::: "memory");
    for (; r != w; r++) belt_on_midi(b, g_nq[r & 63u].b, 3, 0);
    g_nq_r = r;
}

/* PLAY: rows 1-6 are three octaves, two rows each (sharps, then naturals).
 * The pad's note is remembered when it goes down so its release sends the
 * same note-off, whatever happens to the layout in between. */
static bool   g_lp_play = false;
static int8_t g_pad_note[6][8];

static int lp_note_at(uint8_t x, uint8_t y);

static void lp_release_all(void)
{
    for (uint8_t y = 0; y < 6; y++)
        for (uint8_t x = 0; x < 8; x++)
            if (g_pad_note[y][x] >= 0)
            {
                note_push(0x80, (uint8_t)g_pad_note[y][x], 0);
                g_pad_note[y][x] = -1;
            }
}

/* Keyboard layout: pitch class at (x, row) or -1. Row 0 = sharps. */
static int lp_key_at(uint8_t x, uint8_t y)
{
    static const int kSharps[8]   = {-1, 1, 3, -1, 6, 8, 10, -1};
    static const int kNaturals[8] = {0, 2, 4, 5, 7, 9, 11, -1};
    if (y == 0) return kSharps[x];
    if (y == 1) return kNaturals[x];
    return -1;
}

/* PLAY keyboard: MIDI note at (x, y) or -1. Rows 0-1 are the C5 octave,
 * 2-3 C4, 4-5 C3; within each pair, as lp_key_at. */
static int lp_note_at(uint8_t x, uint8_t y)
{
    if (y > 5) return -1;
    const int pc = lp_key_at(x, (uint8_t)(y & 1u));
    return pc < 0 ? -1 : 72 - 12 * (y / 2) + pc;
}

/* Set a PLAY-page selector from a pad: the stored value moves (so it saves
 * and the ring shows it) and the pot re-arms its catch. */
static void lp_set_play(uint8_t pot, int zone, int zones)
{
    float phys[kNumPots];
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();
    pager.SetStored(kPagePlay, pot, ((float)zone + 0.5f) / (float)zones, phys);
}

/* ---- chord sources -----------------------------------------------------------
 *
 * Besides the Launchpad's PLAY pads, one more source of held notes at a time
 * (Settings, page 4 SOURCES, P1 "Chords from"). All of them become note-on /
 * note-off through note_push(), exactly as the pads do; the engine never
 * knows which. Changing the source lets go of every note the old one held.
 *
 *   Pads   no other source (the default)
 *   Seq    the internal chord sequencer: seq-alchemy's own core
 *          (vendor/seq.[ch]) runs its progression at TEMPO (P3), each chord
 *          held as TRIADS or SEVENTHS (P4) in Belt's KEY and SCALE, rooted
 *          in the OCTAVE on the Chord page (P6). Chords and bars are set on
 *          the Launchpad's CHORDS page (top 5). With J3 = Chord clock (P2)
 *          every rising edge on J3 moves to the next chord instead, and the
 *          tempo is not used.
 *   CV     J4.. J7 (P6: how many) are 1 V/oct chord pitches, 0 V = the
 *          OCTAVE's C, each rounded to the nearest semitone. A note changes
 *          when its input moves to a new semitone and stays there; with
 *          J3 = Chord clock the inputs are read only on a J3 edge (sample
 *          and hold). Those jacks stop modulating KEY / RETUNE / AMOUNT /
 *          HARMONY while they carry a chord.
 *          CV cal (P5): On applies each input's learned offset, Off reads
 *          them raw. Learn: have the sender play the reference chord -- J4
 *          0 V, J5 +4/12 V, J6 +7/12 V, J7 +1 V (C E G C) -- then turn P5
 *          to Learn; a quarter second later the offsets are stored in the
 *          slot. Turn it back to On. B3 shows the result for 3 s once
 *          Settings closes: green all four learned, amber some, red none
 *          (an input more than 0.25 V off is a wrong patch, not an error to
 *          learn, and keeps its old offset).
 *   MIDI   notes on the rear header's USART1 (rear_midi.h: the cable, and
 *          why never a straight ribbon), any channel. MIDI clock and
 *          start / stop are ignored in this version.
 *
 * J3 = Chord clock takes J3 away from HARD: hard-tune is then B1 (and the
 * controllers' HARD) only. All of this runs in the 1 ms control poll;
 * nothing new runs in the audio callback but J3's edge count.
 */
static seq_t DSY_SDRAM_BSS g_seq;   /* plain C, ~19 KB: seq_init() fills it */
static cs_held_t g_src_held;        /* what Seq / CV are holding */
static cs_cv_t   g_cv;
static uint32_t  g_midi_held[4];    /* rear-header notes on, by note number */
static int       g_src        = -1; /* the source applied: kSourceLabels */
static int       g_cv_routed  = -1; /* CV inputs taken off the matrix */
static bool      g_seq_run    = true;
static int       g_clk_ci     = 0;  /* chord, when J3 clocks the progression */
static uint32_t  g_edges_seen = 0;
static int       g_cal_prev   = -1, g_cal_n = -1;
static float     g_cal_sum[CS_CV_INPUTS];
static int       g_cal_mask   = -1; /* last Learn's result, to show; -1 none */
static uint32_t  g_cal_show_until = 0;
enum { kSrcPads = 0, kSrcSeq = 1, kSrcCv = 2, kSrcMidi = 3 };

static void mark_dirty(uint32_t now);

static void src_emit(uint8_t status, uint8_t note, uint8_t vel, void* ctx)
{
    (void)ctx;
    note_push(status, note, vel);
}

static void midi_note(uint8_t status, uint8_t note, uint8_t vel)
{
    if (g_src != kSrcMidi || note > 127) return;
    const uint32_t bit = 1u << (note & 31u);
    if (status == 0x90) g_midi_held[note >> 5] |= bit;
    else
    {
        if (!(g_midi_held[note >> 5] & bit)) return;
        g_midi_held[note >> 5] &= ~bit;
    }
    note_push(status, note, vel);
}

static void midi_release_all(void)
{
    for (int i = 0; i < 128; i++)
        if (g_midi_held[i >> 5] & (1u << (i & 31)))
            note_push(0x80, (uint8_t)i, 0);
    memset(g_midi_held, 0, sizeof g_midi_held);
}

/* The progression the CHORDS page edits lives in the extras (saved); the
 * key, scale and chord size follow the panel and Settings. */
static void prog_from_extras(void)
{
    g_seq.prog.count = extras.prog_count;
    for (int i = 0; i < SEQ_MAX_CHORDS; i++)
    {
        g_seq.prog.chord[i].degree = extras.prog_degree[i];
        g_seq.prog.chord[i].bars   = extras.prog_bars[i];
    }
}

static void prog_edited(uint32_t now)
{
    extras.prog_count = g_seq.prog.count;
    for (int i = 0; i < SEQ_MAX_CHORDS; i++)
    {
        extras.prog_degree[i] = g_seq.prog.chord[i].degree;
        extras.prog_bars[i]   = g_seq.prog.chord[i].bars;
    }
    if (g_clk_ci >= g_seq.prog.count) g_clk_ci = 0;
    mark_dirty(now);
}

/* The chord the sequencer is on now, -1 when stopped. */
static int seq_chord_now(void)
{
    if (!g_seq_run) return -1;
    return g_j3_clock ? g_clk_ci : seq_chord_index(&g_seq);
}

static void seq_set_run(bool run)
{
    g_seq_run = run;
    if (run) { seq_play(&g_seq); g_clk_ci = 0; }
    else     seq_stop(&g_seq);
}

/* J4..J7 belong to the chord while CV is the source; otherwise back to
 * their knobs. Re-pointing at runtime is the matrix's documented way. */
static void cv_route(int taken)
{
    if (taken == g_cv_routed) return;
    g_cv_routed = taken;
    VirtualKnob* const dest[CS_CV_INPUTS] = {&key, &retune, &amount, &harmony};
    for (int i = 0; i < CS_CV_INPUTS; i++)
    {
        if (i < taken) cv_matrix.Jack((uint8_t)(1 + i)).Off();
        else           cv_matrix.Jack((uint8_t)(1 + i)).To(*dest[i]);
    }
}

static void chords_poll(uint32_t now)
{
    const int  src   = (int)source_s.Value();
    const int  base  = 36 + 12 * (int)octave_s.Value();      /* C2 = 36 */
    const int  n_cv  = 1 + (int)cv_ins_s.Value();
    const int  cal   = (int)cv_cal_s.Value();
    g_j3_clock = (int)j3_mode_s.Value() == 1;

    const uint32_t e     = g_gate_edges;
    const uint32_t edges = e - g_edges_seen;
    g_edges_seen = e;

    /* MIDI bytes are always drained; they reach the engine only when MIDI
     * is the source (midi_note). */
    rearmidi::Poll(midi_note);

    if (src != g_src)
    {
        cs_release(&g_src_held, src_emit, nullptr);
        midi_release_all();
        g_src = src;
        if (src == kSrcSeq) seq_set_run(g_seq_run);
        cs_cv_init(&g_cv);
    }
    cv_route(src == kSrcCv ? n_cv : 0);

    /* CV inputs, with or without the learned offsets; Learn averages the
     * raw inputs over 256 polls (a quarter second) whatever the source. */
    float raw[CS_CV_INPUTS], v[CS_CV_INPUTS];
    for (int i = 0; i < CS_CV_INPUTS; i++)
    {
        raw[i] = hw.cv_jacks[1 + i].Volts();
        v[i]   = raw[i] - (cal != 0 ? (float)extras.cv_off_mv[i] / 1000.0f : 0.0f);
    }
    /* Only a turn INTO Learn learns: a slot saved with Learn showing must
     * not re-learn at boot from whatever is patched then. */
    if (g_cal_prev < 0) g_cal_prev = cal;
    if (cal == 2 && g_cal_prev != 2)
    {
        g_cal_n = 0;
        for (int i = 0; i < CS_CV_INPUTS; i++) g_cal_sum[i] = 0.0f;
    }
    g_cal_prev = cal;
    if (g_cal_n >= 0)
    {
        for (int i = 0; i < CS_CV_INPUTS; i++) g_cal_sum[i] += raw[i];
        if (++g_cal_n == 256)
        {
            float avg[CS_CV_INPUTS];
            for (int i = 0; i < CS_CV_INPUTS; i++) avg[i] = g_cal_sum[i] / 256.0f;
            g_cal_mask       = (int)cs_cal_learn(avg, CS_CV_INPUTS, extras.cv_off_mv);
            g_cal_show_until = 0;   /* armed: shown once Settings closes */
            g_cal_n          = -1;
            mark_dirty(now);
        }
    }

    uint8_t notes[CS_MAX_NOTES];
    if (src == kSrcSeq)
    {
        g_seq.prog.root  = (uint8_t)(k_key.last < 0 ? 0 : k_key.last);
        g_seq.prog.scale = (uint8_t)cs_seq_scale(k_scale.last < 0 ? 1 : k_scale.last);
        g_seq.prog.tones = (uint8_t)(3 + (int)tones_s.Value());
        g_seq.bpm        = 40.0f + 200.0f * tempo_k.Value();
        if (g_j3_clock)
        {
            if (edges && g_seq.prog.count)
                g_clk_ci = (int)((g_clk_ci + edges) % g_seq.prog.count);
        }
        else
            seq_process(&g_seq, now * 1000u, nullptr);   /* ms clock: GetUs() wraps early */
        const int ci = seq_chord_now();
        const int n  = ci < 0 ? 0 : cs_chord_notes(&g_seq.prog, ci, base, notes);
        cs_hold(&g_src_held, notes, n, 100, src_emit, nullptr);
    }
    else if (src == kSrcCv)
    {
        static int ncv_held = 0;
        const bool moved = cs_cv_poll(&g_cv, v, n_cv) || n_cv != ncv_held;
        ncv_held = n_cv;
        if (g_j3_clock ? edges != 0 : (moved || g_src_held.n == 0))
            cs_hold(&g_src_held, notes, cs_cv_notes(&g_cv, n_cv, base, notes), 100,
                    src_emit, nullptr);
    }
}

/* ---- Gamepad punch effects (Haute42 in XInput mode, through the hub) --------
 *
 * The output runs through punch_fx.h in Launchpad mode; every gamepad button
 * holds an effect (the last pressed wins, release goes back to dry):
 *
 *   top row     X Stutter 1/4   Y Stutter 1/8   RB Stutter 1/16   LB Buzz
 *   bottom row  A Reverse       B Tape Stop     RT Half Speed     LT Echo
 *   directions  Left Low-pass   Down High-pass  Right Crush       Up Gate
 *   L3 HOLD on / off (no effect)    R3 Drive
 * * Tempo-synced to 120 BPM (Belt has no tempo of its own).
 */
#define PFX_RING_FRAMES 96000u   /* 2 s: two beats at 60 BPM */
#define PFX_ECHO_FRAMES 48000u   /* 1 s: a dotted 1/8 at 45 BPM */
static float DSY_SDRAM_BSS g_pfx_ring[2 * PFX_RING_FRAMES];
static float DSY_SDRAM_BSS g_pfx_echo[2 * PFX_ECHO_FRAMES];
static pfx_t g_pfx;

/* index = pad:: bit (XInput wButtons, then LT/RT) */
static const int8_t kPadPfx[18] = {
    PFX_GATE, PFX_HIGH_PASS, PFX_LOW_PASS, PFX_CRUSH,      /* Up Down Left Right */
    -1, -1, -1, PFX_DRIVE,                                 /* Start Back L3 R3   */
    PFX_BUZZ, PFX_STUTTER_16, -1, -1,                      /* LB RB Guide -      */
    PFX_REVERSE, PFX_TAPE_STOP, PFX_STUTTER_4, PFX_STUTTER_8, /* A B X Y         */
    PFX_ECHO, PFX_HALF_SPEED,                              /* LT RT              */
};
static constexpr int kPadHoldBit = 6;   /* L3: toggles HOLD */
static uint32_t g_pad_prev = 0;
static int      g_pad_fx   = -1;

static void pad_poll(void)
{
    const uint32_t b = pad::Buttons();
    const uint32_t pressed = b & ~g_pad_prev, released = g_pad_prev & ~b;
    g_pad_prev = b;
    if ((pressed >> kPadHoldBit & 1u) && !settings.IsActive()) g_hold_latch = !g_hold_latch;
    for (int i = 0; i < 18; i++)
        if ((pressed >> i & 1u) && kPadPfx[i] >= 0) g_pad_fx = kPadPfx[i];
    for (int i = 0; i < 18; i++)
    {
        if (!(released >> i & 1u) || kPadPfx[i] != g_pad_fx) continue;
        g_pad_fx = -1;
        for (int j = 0; j < 18; j++)
            if ((b >> j & 1u) && kPadPfx[j] >= 0) g_pad_fx = kPadPfx[j];
    }
    pfx_hold(&g_pfx, settings.IsActive() ? -1 : g_pad_fx);
}

/* CHORDS page: a pad or side button pressed (see the map above). */
static bool g_lp_chords = false;
static void chords_pad(const lp::Event& e, uint32_t now)
{
    seq_prog_t& pr = g_seq.prog;
    if (e.kind == lp::Kind::Side)
    {
        if (e.y == 0) seq_set_run(!g_seq_run);
        if (e.y == 1 && pr.count > 1) { pr.count--; prog_edited(now); }
        if (e.y == 2 && pr.count < SEQ_MAX_CHORDS) { pr.count++; prog_edited(now); }
        return;
    }
    if (e.y < 7)
    {
        pr.chord[e.x].degree = (int8_t)(6 - e.y);
        if (e.x >= pr.count) pr.count = (uint8_t)(e.x + 1);
        prog_edited(now);
    }
    else if (e.x < pr.count)
    {
        uint8_t& b = pr.chord[e.x].bars;
        b = b >= 8 ? 1 : (b >= 4 ? 8 : (b >= 2 ? 4 : 2));
        prog_edited(now);
    }
}

static void lp_poll(uint32_t now)
{
    lp::Poll(now);
    pad_poll();
    lp::Event e;
    const bool live = !settings.IsActive();
    while (lp::PopEvent(&e))
    {
        if (e.kind == lp::Kind::Top)
        {
            if (e.x == 0) g_lp_hard_down = e.down && live;
            if (e.x == 1) g_lp_mute_down = e.down && live;
            if (e.x == 2 && e.down && live)
            {
                if (g_lp_play) lp_release_all();
                g_lp_play   = !g_lp_play;   /* from CHORDS (play off): PLAY */
                g_lp_chords = false;
            }
            if (e.x == 3 && e.down && live) g_hold_latch = !g_hold_latch;
            if (e.x == 4 && e.down && live)
            {
                if (g_lp_play) lp_release_all();
                g_lp_play   = false;
                g_lp_chords = !g_lp_chords;
            }
            continue;
        }
        if (g_lp_chords)
        {
            if (live && e.down && e.kind != lp::Kind::Top) chords_pad(e, now);
            continue;
        }
        if (g_lp_play && e.kind == lp::Kind::Grid && e.y < 6)
        {
            /* A release always goes through, Settings open or not, so a pad
             * let go while Settings is up cannot leave a note stuck on. */
            int8_t& held = g_pad_note[e.y][e.x];
            if (!e.down)
            {
                if (held >= 0) note_push(0x80, (uint8_t)held, 0);
                held = -1;
            }
            else if (live && held < 0)
            {
                const int n = lp_note_at(e.x, e.y);
                if (n >= 0)
                {
                    held = (int8_t)n;
                    note_push(0x90, (uint8_t)n, 100);   /* the Mini MK3 has no velocity */
                }
            }
            continue;
        }
        if (!live || !e.down || e.kind != lp::Kind::Grid) continue;
        const int pc = lp_key_at(e.x, e.y);
        if (pc >= 0) lp_set_play(kPotTopLeft, pc, 12);
        if (e.y == 3 || (e.y == 4 && e.x == 0))
        {
            const int sc = (e.y == 3) ? e.x : 8;
            lp_set_play(kPotTopRight, sc, 9);
        }
    }
}

/* ---- Launch Control XL -----------------------------------------------------
 *
 *   faders 1-4    RETUNE, AMOUNT, HARMONY, FORMANT (the PLAY knobs)
 *   faders 5-6    DOUBLER, SPREAD (SETUP)
 *   top knobs 1-4 VOICE 1-4 intervals (SETUP)
 *   middle knobs  1 KEY, 2 SCALE
 *   upper row     1 HARD, 2 MUTE: hold for momentary, tap to latch;
 *                 3 HOLD on / off
 * All through the pages' stored values, so the pots catch and it saves.
 */
static void mark_dirty(uint32_t now);
static bool g_xl_hard_down = false, g_xl_mute_down = false;

static void xl_frame(uint32_t now)
{
    if (!xl::Connected()) return;
    float phys[kNumPots];
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();
    uint8_t v;
    static const uint8_t kFaderPage[6] = {kPagePlay, kPagePlay, kPagePlay, kPagePlay, kPageSetup, kPageSetup};
    static const uint8_t kFaderPot[6]  = {kPotMiddleLeft, kPotMiddleRight, kPotBottomLeft,
                                          kPotBottomRight, kPotBottomLeft, kPotBottomRight};
    for (uint8_t c = 0; c < 6; c++)
        if (xl::Fader(c, &v))
        {
            pager.SetStored(kFaderPage[c], kFaderPot[c], (float)v / 127.0f, phys);
            mark_dirty(now);
        }
    static const uint8_t kVoicePot[4] = {kPotTopLeft, kPotTopRight, kPotMiddleLeft, kPotMiddleRight};
    for (uint8_t c = 0; c < 4; c++)
        if (xl::Knob(0, c, &v))
        {
            pager.SetStored(kPageSetup, kVoicePot[c], (float)v / 127.0f, phys);
            mark_dirty(now);
        }
    if (xl::Knob(1, 0, &v)) { pager.SetStored(kPagePlay, kPotTopLeft, (float)v / 127.0f, phys);  mark_dirty(now); }
    if (xl::Knob(1, 1, &v)) { pager.SetStored(kPagePlay, kPotTopRight, (float)v / 127.0f, phys); mark_dirty(now); }

    xl::Button b;
    const bool live = !settings.IsActive();
    while (xl::PopButton(&b))
    {
        if (b.row != 0) continue;
        if (b.col == 0) g_xl_hard_down = b.down && live;
        if (b.col == 1) g_xl_mute_down = b.down && live;
        if (b.col == 2 && b.down && live) g_hold_latch = !g_hold_latch;
    }

    for (uint8_t c = 0; c < 8; c++)
    {
        uint8_t up = xl::kOff;
        if (c == 0) up = G_HARD ? xl::kRed : xl::kRedDim;
        if (c == 1) up = G_MUTED ? xl::kAmber : xl::kAmberDim;
        if (c == 2) up = G_HOLD ? xl::kGreen : xl::kGreenDim;
        xl::SetButtonLed(0, c, up);
        xl::SetButtonLed(1, c, xl::kOff);
        xl::SetKnobLed(0, c, c < 4 ? (((G_MASK >> c) & 1) ? xl::kGreen : xl::kGreenDim) : xl::kOff);
        xl::SetKnobLed(1, c, c < 2 ? xl::kAmberDim : xl::kOff);
        xl::SetKnobLed(2, c, xl::kOff);
    }
}

static void lp_paint_tops(void)
{
    lp::SetTop(0, G_HARD ? lp::kRed : lp::kRedDim);
    lp::SetTop(1, G_MUTED ? lp::kAmber : lp::kAmberDim);
    lp::SetTop(2, g_lp_play ? lp::kGreen : lp::kGreenDim);
    lp::SetTop(3, G_HOLD ? lp::kBlue : lp::kBlueDim);
    lp::SetTop(4, g_lp_chords ? lp::kMagenta : lp::kMagentaDim);
    lp::SetLogo(lp::kGreen);
}

static void lp_paint_chords(void)
{
    const seq_prog_t& pr = g_seq.prog;
    const int playing = g_src == kSrcSeq ? seq_chord_now() : -1;
    for (uint8_t x = 0; x < 8; x++)
    {
        const bool in = x < pr.count;
        const int  row = 6 - pr.chord[x].degree;   /* degrees 0..6 have a row */
        for (uint8_t y = 0; y < 7; y++)
        {
            uint8_t c = lp::kOff;
            if (y == row) c = !in ? lp::kGrey : (x == playing ? lp::kGreen : lp::kCyan);
            lp::SetGrid(x, y, c);
        }
        const uint8_t b = pr.chord[x].bars;
        lp::SetGrid(x, 7, !in ? lp::kOff : b >= 8 ? lp::kRed : b >= 4 ? lp::kOrange
                                          : b >= 2 ? lp::kAmber : lp::kAmberDim);
    }
    lp::SetSide(0, g_seq_run ? lp::kGreen : lp::kGreenDim);
    lp::SetSide(1, lp::kGrey);
    lp::SetSide(2, lp::kGrey);
    for (uint8_t y = 3; y < 8; y++) lp::SetSide(y, lp::kOff);
    lp_paint_tops();
}

static void lp_paint(void)
{
    if (!lp::Connected()) return;
    if (g_lp_chords) { lp_paint_chords(); return; }
    for (uint8_t y = 0; y < 8; y++) lp::SetSide(y, lp::kOff);
    const int key_idx   = k_key.last   < 0 ? 0 : k_key.last;
    const int scale_idx = k_scale.last < 0 ? 1 : k_scale.last;
    const int sung      = G_VOICED ? ((G_NOTE10 + 5) / 10 % 12 + 12) % 12 : -1;
    const bool in_tune  = G_CENTS <= 25 && G_CENTS >= -25;
    if (g_lp_play)
    {
        /* PLAY: three octaves, held pads green */
        for (uint8_t y = 0; y < 6; y++)
            for (uint8_t x = 0; x < 8; x++)
            {
                const int pc = lp_key_at(x, (uint8_t)(y & 1u));
                uint8_t   c  = lp::kOff;
                if (pc >= 0)
                {
                    c = lp::kGrey;
                    if (in_scale(pc, key_idx, scale_idx)) c = lp::kBlueDim;
                    if (pc == key_idx) c = lp::kBlue;
                    if (g_pad_note[y][x] >= 0) c = lp::kGreen;
                }
                lp::SetGrid(x, y, c);
            }
    }
    else
    {
        for (uint8_t y = 0; y < 2; y++)
            for (uint8_t x = 0; x < 8; x++)
            {
                const int pc = lp_key_at(x, y);
                uint8_t   c  = lp::kOff;
                if (pc >= 0)
                {
                    c = lp::kGrey;
                    if (in_scale(pc, key_idx, scale_idx)) c = lp::kBlueDim;
                    if (pc == key_idx) c = lp::kBlue;
                    if (pc == sung) c = in_tune ? lp::kGreen : lp::kOrange;
                }
                lp::SetGrid(x, y, c);
            }
        for (uint8_t x = 0; x < 8; x++) lp::SetGrid(x, 3, x == scale_idx ? lp::kMagenta : lp::kMagentaDim);
        lp::SetGrid(0, 4, scale_idx == 8 ? lp::kMagenta : lp::kMagentaDim);
        /* rows KEY does not use: dark (PLAY may have lit them) */
        for (uint8_t x = 0; x < 8; x++)
        {
            lp::SetGrid(x, 2, lp::kOff);
            lp::SetGrid(x, 5, lp::kOff);
            if (x) lp::SetGrid(x, 4, lp::kOff);
        }
    }
    for (uint8_t v = 0; v < 4; v++)
    {
        const bool set = (G_MASK >> v) & 1;
        uint8_t c = !set ? lp::kOff : ((G_VOICED && !G_MUTED) ? lp::kCyan : lp::kCyanDim);
        if (set && G_HOLD_ST && !G_MUTED) c = lp::kBlue;   /* held voices */
        lp::SetGrid(v, 6, c);
    }
    for (uint8_t x = 0; x < 8; x++)
    {
        uint8_t c = lp::kOff;
        if (G_VOICED)
        {
            int pos = (int)lroundf(3.5f + (float)G_CENTS / 50.0f * 4.0f);
            if (pos < 0) pos = 0;
            if (pos > 7) pos = 7;
            if (x == pos) c = in_tune ? lp::kGreen : lp::kOrange;
        }
        lp::SetGrid(x, 7, c);
    }
    lp_paint_tops();
}

/* Host report to /lpdiag.txt once, 17 s after boot, unless a Launchpad came up. */
alignas(32) static ALCHEMY_SDMMC_BSS FIL  s_lpdiag_fil;
alignas(32) static ALCHEMY_SDMMC_BSS char s_lpdiag_buf[4096];
static bool g_lpdiag_done = false;

static void lp_write_report(uint32_t now)
{
    if (g_lpdiag_done || g_lp_stage < 2 || now - g_lp_boot_ms < 17000u) return;
    if (lp::Connected()) { g_lpdiag_done = true; return; }
    if (picker::Busy() || !sd.EnsureMounted(now)) return;
    g_lpdiag_done = true;
    const int n = lp::Report(s_lpdiag_buf, (int)sizeof s_lpdiag_buf);
    if (f_open(&s_lpdiag_fil, "/lpdiag.txt", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT w = 0;
    f_write(&s_lpdiag_fil, s_lpdiag_buf, (UINT)n, &w);
    f_close(&s_lpdiag_fil);
}

/* Buttons still down when Settings closed: ignored until they are let go.
 * The SDK closes Settings on the B2 or B3 press itself, and this poll sees
 * that same press a moment later; without this the press that leaves
 * Settings would also count as a press here. */
static uint8_t g_btn_swallow = 0;

static bool btn_live(uint8_t b)
{
    const bool    down = hw.buttons[b].Pressed();
    const uint8_t bit  = (uint8_t)(1u << b);
    if (g_btn_swallow & bit)
    {
        if (down) return false;
        g_btn_swallow = (uint8_t)(g_btn_swallow & ~bit);
    }
    return down;
}

static void OnPoll(uint32_t now)
{
    if (g_usb_audio && UAC_RebootRequested())
    {
        static uint32_t asked = 0u;
        if (!asked) asked = now ? now : 1u;
        else if (now - asked > 100u)
            System::ResetToBootloader(System::BootloaderMode::DAISY);
    }
    if (g_lp_mode)
    {
        if (g_lp_stage == 0 && now - g_lp_boot_ms > 2000u) g_lp_stage = 1;
        else if (g_lp_stage == 1 && now - g_lp_boot_ms > 2300u)
        {
            lp::Init();
            g_lp_stage = 2;
        }
        else if (g_lp_stage >= 2)
        {
            lp_poll(now);
            g_lp_stage = (uint8_t)(2 + lp::Stage());
        }
    }

    chords_poll(now);   /* held notes keep coming with Settings open */

    if (settings.IsActive())
    {
        /* Settings owns the buttons; drop momentaries, keep latches. */
        tg_hard.Reset();
        tg_mute.Reset();
        b2g.Reset();
        apply_hard(tg_hard.latch || j3_hard());
        apply_mute(tg_mute.latch);
        apply_hold(g_hold_latch || g_hold_gate);
        g_btn_swallow = (uint8_t)((1u << kButtonB1) | (1u << kButtonB2) | (1u << kButtonB3));
        return;
    }
    const bool hard = tg_hard.Poll(btn_live(kButtonB1) || g_lp_hard_down || g_xl_hard_down, now)
                      || j3_hard();
    /* B2 stands down while B3 is held: B2+B3 held two seconds is the
     * Settings chord, and B2's own gesture would mute the harmonies on the
     * way in and, if B3 landed inside the tap window, flip the latch. */
    const bool b3   = btn_live(kButtonB3);
    if (b3) { tg_mute.Reset(); b2g.Reset(); }
    else    b2g.Poll(btn_live(kButtonB2), now, &tg_mute.latch);
    const bool mute = b3 ? tg_mute.latch
                         : tg_mute.Poll(g_lp_mute_down || g_xl_mute_down, now);
    apply_hard(hard);
    apply_mute(mute);
    apply_hold(g_hold_latch || g_hold_gate);
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static bool     g_dirty       = false;
static uint32_t g_dirty_since = 0;
static float    g_saved_peak  = 0.0f;
static bool     prev_settings = false;
static int      applied_flex = -1, applied_humanize = -1, applied_wet = -1;
static int      applied_lead = -1, applied_vel = -1, applied_midi = -1;
static int      applied_hold_mode = -1, applied_j8 = -1;

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
        /* The Chord page: what held notes (Launchpad PLAY pads) do. */
        const int ld = settings_pct(lead_k), vs = settings_pct(vel_k);
        const int mm = (int)midi_mode_s.Value();
        if (ld != applied_lead) { applied_lead = ld; set_param_int("lead",      ld); }
        if (vs != applied_vel)  { applied_vel  = vs; set_param_int("vel_sens",  vs); }
        if (mm != applied_midi) { applied_midi = mm; set_param_int("midi_mode", mm); }
        const int hm = (int)hold_mode_s.Value(), j8 = (int)j8_mode_s.Value();
        if (hm != applied_hold_mode) { applied_hold_mode = hm; set_param_int("hold_mode", hm); }
        if (j8 != applied_j8)
        {
            /* J8: the FORMANT knob's CV, or the HOLD gate. Re-pointing a jack
             * at runtime is the matrix's documented way (cv_matrix.h). */
            applied_j8 = j8;
            if (j8) cv_matrix.Jack(5).Custom(OnHoldGate, nullptr);
            else
            {
                cv_matrix.Jack(5).To(formant);
                g_hold_gate = false;
            }
        }
    }

    /* Engine readback, one snprintf per frame: "note10:cents:voiced:mask". */
    {
        char buf[64];
        int  n10 = 0, cents = 0, voiced = 0, mask = 0, held = 0, hard = 0, hold = 0;
        const int got = belt_get_param(B, "status", buf, sizeof buf) > 0
            ? sscanf(buf, "%d:%d:%d:%d:%d:%d:%d", &n10, &cents, &voiced, &mask, &held, &hard, &hold)
            : 0;
        if (got >= 4)
        {
            G_NOTE10 = n10;
            G_CENTS  = cents;
            G_VOICED = voiced;
            G_MASK   = mask;
        }
        if (got == 7) G_HOLD_ST = hold;
    }

    /* Worst block this session. Clamped: an overrunning callback can report
     * over 100%, and the readout wants 0..1. */
    {
        float mx = cpu.GetMaxCpuLoad();
        if (mx > 1.0f) mx = 1.0f;
        if (mx > extras.cpu_peak) extras.cpu_peak = mx;
        if (extras.cpu_peak > g_saved_peak + 0.05f) mark_dirty(now);

        /* The worst SMOOTHED load this session: one heavy block and a
         * callback that is always near the limit read the same on the peak,
         * and only this tells them apart. Read over HostLink (getlive). */
        float av = cpu.GetAvgCpuLoad();
        if (av > 1.0f) av = 1.0f;
        if (av > extras.cpu_avg) extras.cpu_avg = av;
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
    if (g_lp_mode) { lp_paint(); xl_frame(now); lp_write_report(now); }

    /* The USB port only takes effect at power-up, so a change is saved as
     * soon as Settings closes -- nobody should have to wait before cycling. */
    /* A USB port change goes to the card for every firmware, as soon as
     * Settings closes, independently of this firmware's autosave. */
    static int shared_written = -2;
    if (shared_written == -2) shared_written = (int)usb_port.Value();
    if (!sact && (int)usb_port.Value() != shared_written && !picker::Busy()
        && usbshared::Save(sd, (int)usb_port.Value(), now))
        shared_written = (int)usb_port.Value();

    static int saved_usb = -1;
    if (saved_usb < 0) saved_usb = (int)usb_port.Value();
    const bool usb_changed = !sact && (int)usb_port.Value() != saved_usb;

    if (g_dirty && (now - g_dirty_since >= 5000u || usb_changed) && !sact && !picker::Busy()
        && !hw.buttons[kButtonB1].Pressed() && !hw.buttons[kButtonB2].Pressed()
        && !hw.buttons[kButtonB3].Pressed())
    {
        presets.Save(kHomeSlot);
        saved_usb    = (int)usb_port.Value();
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
 *                    dim when no voice has an interval; blue while HOLD
 *                    is on (dim if muted); red when the audio callback is
 *                    above 80%
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
    else if (G_HOLD)                 b2 = G_MUTED ? LedPanel::Rgb{0x04, 0x18, 0x40} : kHold;
    else if (G_MUTED)                b2 = kGrey;
    else if (G_MASK)                 b2 = kPurple;
    else                             b2 = kIdle;
    if (g_lp_mode)
    {
        static const LedPanel::Rgb kStage[6] = {
            {0x40, 0x00, 0x40}, {0x00, 0x00, 0xFF}, {0x00, 0xC0, 0xC0},
            {0xFF, 0xC0, 0x00}, {0xFF, 0x00, 0x00}, {0x00, 0xFF, 0x00}};
        if (g_lp_stage < 5) b2 = kStage[g_lp_stage];   /* running: B2 is B2 again */
    }
    if (g_lp_mode && pad::Buttons()) b2 = {0xFF, 0xFF, 0xFF}; /* a gamepad button held */
    if (g_usb_audio)
    {
        static const LedPanel::Rgb kUac[4] = {
            {0x40, 0x00, 0x40},   /* not started                   */
            {0x00, 0xC0, 0xC0},   /* waiting for a computer        */
            {0xFF, 0xC0, 0x00},   /* connected, no stream open     */
            {0x00, 0xFF, 0x00}};  /* streaming                     */
        const uint8_t st = UAC_State();
        b2 = kUac[st < 4 ? st : 0];
    }
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2));

    if (g_cal_mask >= 0)
    {
        /* The last CV Learn's result, for 3 s after Settings closes. */
        if (!g_cal_show_until) g_cal_show_until = t_ms + 3000u;
        if (t_ms < g_cal_show_until)
        {
            L.SetButtonPair(kButtonB3, L.ScaleGlobal(g_cal_mask == 0xF ? kGreen
                                                     : g_cal_mask ? kAmber : kRed));
            return;
        }
        g_cal_mask = -1;
    }
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

    /* Audio interface mode: the jacks belong to the computer. */
    if (g_usb_audio)
    {
        UAC_Process(in, out, size);
        cpu.OnBlockEnd();
        return;
    }

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

    note_drain(B);   /* pad notes from the control loop, before the block */
    belt_process(B, bufi, bufi, (int)size);

    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = (float)bufi[2 * i] * k;
        out[1][i] = (float)bufi[2 * i + 1] * k;
    }

    /* The gamepad's punch effects, last on the output. */
    if (g_lp_mode) pfx_process(&g_pfx, out[0], out[1], (uint32_t)size);

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

    memset(g_pad_note, -1, sizeof g_pad_note);   /* no pad held */
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
    usb_port = settings.Page(kSettingsFirmware).Pot(5)
        .Selector(kUsbLabels).Default(0)
        .Ident("usb").Name("USB port")
        .Help("**Mac**: the front USB-C is HostLink, for the web programmer, "
              "presets and the card. **Launchpad**: the Lab is the USB host "
              "for a Launchpad Mini MK3 and a Launch Control XL (5 V from a "
              "powered dongle). **Audio**: the Lab is a USB audio interface, "
              "2 in (J1/J2) and 2 out (J9/J10) at 48 kHz, and the engine is "
              "bypassed. From the next power-up; hold B1 while powering up "
              "for Mac mode once.");
    settings.UseBrightness();
    settings.UsePresets(presets);
    flex_k = settings.Page(kSettingsMain).Pot(1).Knob().Default(0.0f)
        .Ident("flex").Name("Flex").Color(kColRetune)
        .Help("How far from any scale note the singing may stray before "
              "correction lets go of it. 0 corrects everything.");
    /* Settings page 3, Chord: what held notes do (Launchpad PLAY pads now;
     * the chord sequencer and the front/back links later, all through the
     * same belt_on_midi). B1 steps Main -> Firmware -> Chord. */
    settings.Page(kSettingsChord).Name("Chord")
        .Help("What held notes do. Hold them on the Launchpad in PLAY (the "
              "third top button), or bring them from the sequencer, CV or "
              "rear-header MIDI (the Sources page). With **MIDI notes** on Harmony the harmony "
              "voices sing the held notes; turn **Lead** to 0 and only they "
              "sound -- every note your own voice, re-pitched: Hide and Seek.");
    midi_mode_s = settings.Page(kSettingsChord).Pot(0)
        .Selector(kMidiModeLabels).Default(1)
        .Ident("midi_mode").Name("MIDI notes")
        .Help("**Off**: held notes are ignored. **Harmony** (default): each "
              "held note takes a harmony voice, up to four, newest wins. "
              "**Target**: the newest held note becomes the pitch the lead "
              "is corrected to -- play the melody, sing roughly, land on it.");
    lead_k = settings.Page(kSettingsChord).Pot(1).Knob().Default(1.0f)
        .Ident("lead").Name("Lead").Color(kColKey)
        .Help("The lead: the corrected voice, the dry voice and the doubler "
              "together. 100 is Belt as always. 0 is chord only: just the "
              "harmony voices on the held notes, the Hide and Seek sound. "
              "Set the Setup page's Voice 1-4 intervals to Off so only held "
              "notes sing.");
    hold_mode_s = settings.Page(kSettingsChord).Pot(2)
        .Selector(kHoldModeLabels).Default(0)
        .Ident("hold_mode").Name("Hold")
        .Help("What HOLD does (B2 held 0.6 s, Launchpad top 4, XL upper 3, "
              "gamepad L3, or J8). **Freeze** (default): the harmony voices "
              "keep the chord you were singing and sustain it as a pad, "
              "even when you stop. **Lock**: they keep their notes but sing "
              "with your live voice -- your words on a fixed chord -- and "
              "go quiet when you do.");
    j8_mode_s = settings.Page(kSettingsChord).Pot(3)
        .Selector(kJ8Labels).Default(0)
        .Ident("j8").Name("J8")
        .Help("**Formant** (default): J8 is the FORMANT knob's CV. **Hold "
              "gate**: J8 holds while its gate is high -- a footswitch "
              "through a gate converter, or a sequencer.");
    vel_k = settings.Page(kSettingsChord).Pot(4).Knob().Default(0.5f)
        .Ident("vel_sens").Name("Vel sens").Color(kColAmount)
        .Help("How much a held note's velocity sets its voice's level. The "
              "Launchpad Mini MK3 has no velocity (its pads send 100), nor do "
              "the sequencer and CV chords; rear-header MIDI does.");
    octave_s = settings.Page(kSettingsChord).Pot(5)
        .Selector(kOctaveLabels).Default(1)
        .Ident("chord_oct").Name("Octave")
        .Help("Where the sequencer's and the CV inputs' chords sit: the "
              "sequencer's chord roots start at this C, and 0 V on a CV "
              "chord input is this C (1 V/oct up from it). Keep the chord "
              "near the voice -- the harmony voices are your voice "
              "re-pitched.");

    /* Settings page 4, Sources: where else held notes come from. */
    settings.Page(kSettingsSources).Name("Sources")
        .Help("Where held notes come from besides the Launchpad's PLAY pads: "
              "**Seq**, the internal chord sequencer (its chords on the "
              "Launchpad's CHORDS page, top 5); **CV**, 1 V/oct chord pitches "
              "on J4-J7; **MIDI**, notes on the rear header from another Lab. "
              "With MIDI notes on Harmony (Chord page) the harmony voices "
              "sing them.");
    source_s = settings.Page(kSettingsSources).Pot(0)
        .Selector(kSourceLabels).Default(0)
        .Ident("chord_src").Name("Chords from")
        .Help("**Pads** (default): the Launchpad's PLAY pads only. **Seq**: "
              "the internal chord sequencer. **CV**: J4-J7 (P6 says how many) "
              "are chord pitches and stop modulating their knobs. **MIDI**: "
              "notes on the rear header (USART1, header pin 7; never a "
              "straight ribbon -- see the manual). A change lets go of every "
              "note the old source held.");
    j3_mode_s = settings.Page(kSettingsSources).Pot(1)
        .Selector(kJ3Labels).Default(0)
        .Ident("j3_mode").Name("J3")
        .Help("**Hard gate** (default): a gate on J3 punches hard-tune. "
              "**Chord clock**: each rising edge on J3 moves the sequencer to "
              "its next chord (the tempo is not used), or, with CV, reads the "
              "chord inputs (sample and hold). Hard-tune is then B1 only.");
    tempo_k = settings.Page(kSettingsSources).Pot(2).Knob().Default(0.4f)
        .Ident("chord_bpm").Name("Tempo").Color(kColRetune)
        .Help("The sequencer's tempo, 40-240 BPM (120 at 0.4). A chord lasts "
              "its bars of 4/4 at this tempo.");
    tones_s = settings.Page(kSettingsSources).Pot(3)
        .Selector(kTonesLabels).Default(0)
        .Ident("chord_tones").Name("Chord")
        .Help("The sequencer's chords as **Triads** (three notes) or "
              "**Sevenths** (four: every harmony voice).");
    cv_cal_s = settings.Page(kSettingsSources).Pot(4)
        .Selector(kCvCalLabels).Default(1)
        .Ident("cv_cal").Name("CV cal")
        .Help("**On** (default): the CV chord inputs take off each one's "
              "learned offset. **Off**: raw. **Learn**: with the sender "
              "playing the reference chord (J4 0 V, J5 +4/12 V, J6 +7/12 V, "
              "J7 +1 V: C E G C), turn to Learn; a quarter second later the "
              "offsets are saved. Then back to On. B3 shows the result after "
              "Settings closes: green all, amber some, red none.");
    cv_ins_s = settings.Page(kSettingsSources).Pot(5)
        .Selector(kCvInsLabels).Default(3)
        .Ident("cv_ins").Name("CV ins")
        .Help("How many CV chord inputs, from J4 up. The rest keep modulating "
              "their knobs. An unpatched input reads 0 V -- the Octave's C -- "
              "so leave it out here rather than unpatched.");
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

    /* The chord sequencer: seq-alchemy's defaults, then the saved
     * progression. MIDI in on the rear header listens from now on. */
    seq_init(&g_seq, 1u);
    prog_from_extras();
    cs_cv_init(&g_cv);
    rearmidi::Init();

    /* Report last session's peak on the P1 ring, then start recording this
     * one. Audio is already passing through during the readout. */
    g_boot_peak      = extras.cpu_peak;
    extras.cpu_peak  = 0.0f;
    extras.cpu_avg   = 0.0f;
    g_saved_peak     = 0.0f;
    g_readout_until  = System::GetNow() + 2500u;

    /* The front port's role is one setting for every firmware on the card:
     * adopt the card's value, so a picker switch keeps Launchpad mode. */
    const int shared_usb = usbshared::Load(sd, 500);
    if (shared_usb >= 0 && shared_usb != (int)usb_port.Value())
        usb_port.Default((uint8_t)shared_usb);

    /* B1 held through power-up: Mac mode this boot. Read over ~20 ms so
     * the debouncer has settled whatever came before. */
    bool force_mac = false;
    for (int i = 0; i < 20; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    force_mac = hw.buttons[kButtonB1].Pressed();
    g_lp_mode    = (int)usb_port.Value() == 1 && !force_mac;
    g_usb_audio  = (int)usb_port.Value() == 2 && !force_mac;
    g_lp_boot_ms = System::GetNow();
    if (g_usb_audio) UAC_Start("Alchemy Lab");

    pfx_init(&g_pfx, g_pfx_ring, PFX_RING_FRAMES, g_pfx_echo, PFX_ECHO_FRAMES, 48000.0f);
    hw.StartAudio(AudioCallback);
    cpu.Reset();

    loop.Use(pager)
        .Use(settings)
        .Use(cv_matrix)
        .Use(play_page)
        .Use(setup_page)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);
    if (!g_lp_mode && !g_usb_audio) loop.Use(host);

    for (;;) loop.Tick();
}
