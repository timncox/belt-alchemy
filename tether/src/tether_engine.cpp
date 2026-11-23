/* tether_engine -- see tether_engine.h. */
#include "tether_engine.h"

#include <math.h>
#include <string.h>

namespace tether
{

float EnvSeconds(float x)
{
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return 0.001f * powf(4000.0f, x);   /* 1 ms .. 4 s */
}
static float env_knob(float s) { return logf(s / 0.001f) / logf(4000.0f); }

static const char* const kLayoutNames[kNumLayouts + 1] = {"BAND", "CLOCK", "CV IN", "AUDIO", "CUSTOM"};
const char* LayoutName(int l) { return l >= 0 && l < kNumLayouts ? kLayoutNames[l] : kLayoutNames[kNumLayouts]; }

/*                                      J3          J4         J5            J6
 *                                      J7          J8         J9            J10 */
static const uint8_t kLayouts[kNumLayouts][kJacks] = {
    /* BAND: bass gate, lead gate, HATS, 16ths clock out; KICK, SNARE on
     * the fast DACs; bass and lead pitch on the 24-bit codec outputs. */
    {kFnGateA, kFnGateB, kFnTrig1 + 2, kFnClk16, kFnTrig1, kFnTrig1 + 1, kFnPitchA, kFnPitchB},
    /* CLOCK (0.1 / 0.2): gates A and B, 16ths, reset, KICK, SNARE, pitch A and B. */
    {kFnGateA, kFnGateB, kFnClk16, kFnReset, kFnTrig1, kFnTrig1 + 1, kFnPitchA, kFnPitchB},
    /* CV IN (0.4.0): BAND with FILTER and DELAY coming in on J5 and J6 in
     * place of HATS and the 16ths. */
    {kFnGateA, kFnGateB, kFnCvIn1, kFnCvIn1 + 2, kFnTrig1, kFnTrig1 + 1, kFnPitchA, kFnPitchB},
    /* AUDIO (0.5.0): CV IN's jacks, with the FM-1's sound (USB audio) on J9
     * and J10 in place of the pitches. */
    {kFnGateA, kFnGateB, kFnCvIn1, kFnCvIn1 + 2, kFnTrig1, kFnTrig1 + 1, kFnAudioL, kFnAudioR},
};

int JackZones(int j) { return j < kNumInJacks ? kFnAudioL : kFnCvIn1 + 2; }

uint8_t JackFnOfZone(int j, int z)
{
    if (z < 0) z = 0;
    if (z >= JackZones(j)) z = JackZones(j) - 1;
    if (j < kNumInJacks || z < kFnCvIn1) return (uint8_t)z;
    return (uint8_t)(kFnAudioL + (z - kFnCvIn1));
}

int JackZoneOfFn(int j, uint8_t fn)
{
    if (j >= kNumInJacks && IsAudio(fn)) return kFnCvIn1 + (fn - kFnAudioL);
    return fn < JackZones(j) ? fn : 0;
}

void SetLayout(uint8_t jack[kJacks], int l)
{
    if (l < 0 || l >= kNumLayouts) return;
    memcpy(jack, kLayouts[l], kJacks);
}

int LayoutOf(const uint8_t jack[kJacks])
{
    for (int l = 0; l < kNumLayouts; l++)
        if (!memcmp(jack, kLayouts[l], kJacks)) return l;
    return -1;
}

void Defaults(MasterParams& m, VoiceParams v[kVoices])
{
    SetLayout(m.jack, kLayoutBand);
    m.trig_ms  = 5.0f;
    m.accent   = 110;     /* PROTECTOR's accented steps */
    m.bend     = 2;
    m.reset_on = kResetStart;
    m.clk_ppqn = kPpqnDefault;
    m.j2_mode  = kJ2Reset;
    /* KICK, SNARE, HATS follow velocity, from 2.5 V (where a fixed-level
     * trigger input such as a kick module's still fires) to 5 V */
    m.drum_vel = 0x07;
    m.drum_trig = 0;   /* held gates: Ultra Perc's patterns follow the gate's length */
    for (int d = 0; d < kDrums; d++) m.drum_floor[d] = 2.5f;
    for (int i = 0; i < kVoices; i++) { m.cal_off[i] = 0.0f; m.cal_scale[i] = 0.0f; }
    /* A: PROTECTOR's BASS (channel 5), B: its LEAD (6), C: its PAD (7). */
    for (int i = 0; i < kVoices; i++)
    {
        v[i].chan      = (uint8_t)(4 + i);
        v[i].gate_mode = kGate;
        v[i].note_mode = kLegato;
        v[i].glide     = 0.0f;
        v[i].octave    = 0;
        v[i].transpose = 0;
    }
    v[2].note_mode = kLow;   /* a chord on the pad: its lowest note */
    /* Envelopes: a short bass, a lead that holds, a slow pad. */
    const float env[kVoices][4] = {{0.001f, 0.25f, 0.4f, 0.08f},
                                   {0.005f, 0.3f, 0.6f, 0.3f},
                                   {0.4f, 0.8f, 0.8f, 1.2f}};
    for (int i = 0; i < kVoices; i++)
    {
        v[i].att = env_knob(env[i][0]);
        v[i].dec = env_knob(env[i][1]);
        v[i].sus = env[i][2];
        v[i].rel = env_knob(env[i][3]);
    }
}

static const char* const kJackNames[kNumJackFns] = {
    "PITCH A", "PITCH B", "PITCH C", "GATE A", "GATE B", "GATE C", "ENV A", "ENV B", "ENV C",
    "ACC A", "ACC B", "ACC C", "VEL A", "VEL B", "VEL C",
    "CLK 24", "CLK 16", "CLK 8", "CLK 4", "BAR", "RESET", "RUN", "SECTION",
    "TRIG 1", "TRIG 2", "TRIG 3", "TRIG 4", "TRIG 5", "TRIG 6", "TRIG 7", "TRIG 8",
    "IN FILTER", "IN REVERB", "IN DELAY", "IN ECHO", "IN GLITCH", "IN PUMP", "IN RUMBLE", "IN DRIVE",
    "IN COMB", "IN PHASER", "IN FLANGER", "IN CRUSH", "IN SCENE", "IN MORPH", "AUDIO L", "AUDIO R"};

const char* JackName(uint8_t fn) { return fn < kNumJackFns ? kJackNames[fn] : "?"; }

/* ---- state ---------------------------------------------------------------- */

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static void put16(uint8_t* p, int v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
static int  get16(const uint8_t* p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }
static uint8_t u8(float x) { return (uint8_t)lroundf(clampf(x, 0.0f, 1.0f) * 255.0f); }

/* Layout 2 (80 bytes):
 *   0 version   1 voice on the panel   2-9 jobs of J3..J10
 *   10 trig ms  11 accent  12 bend  13 reset on  14 J1 PPQN  15 J2 mode
 *   16-21 calibration offsets A B C (0.1 mV)   22-27 scales (10 ppm)
 *   28-57 voices A B C, ten bytes each: channel, gate, notes, glide, octave+8,
 *         transpose+16, attack, decay, sustain, release
 *   58 drums following velocity (bit per TRIG)   59-66 their floors (20 mV)
 *   67-78 zero   79 check */
void Pack(const MasterParams& m, const VoiceParams v[kVoices], uint8_t sel, uint8_t* out)
{
    memset(out, 0, kStateBytes);
    out[0] = kLayoutVersion;
    out[1] = sel;
    for (int j = 0; j < kJacks; j++) out[2 + j] = m.jack[j];
    out[10] = (uint8_t)lroundf(m.trig_ms);
    out[11] = m.accent;
    out[12] = m.bend;
    out[13] = m.reset_on;
    out[14] = m.clk_ppqn;
    out[15] = m.j2_mode;
    for (int i = 0; i < kVoices; i++)
    {
        put16(&out[16 + 2 * i], (int)lroundf(m.cal_off[i] * 10000.0f));     /* 0.1 mV */
        put16(&out[22 + 2 * i], (int)lroundf(m.cal_scale[i] * 100000.0f));  /* 10 ppm */
    }
    for (int i = 0; i < kVoices; i++)
    {
        uint8_t* p = &out[28 + 10 * i];
        p[0] = v[i].chan;
        p[1] = v[i].gate_mode;
        p[2] = v[i].note_mode;
        p[3] = u8(v[i].glide);
        p[4] = (uint8_t)(v[i].octave + 8);
        p[5] = (uint8_t)(v[i].transpose + 16);
        p[6] = u8(v[i].att);
        p[7] = u8(v[i].dec);
        p[8] = u8(v[i].sus);
        p[9] = u8(v[i].rel);
    }
    out[58] = m.drum_vel;
    for (int d = 0; d < kDrums; d++) out[59 + d] = (uint8_t)lroundf(clampf(m.drum_floor[d], 0.0f, kDrumFloorMax) * 50.0f);
    out[67] = m.drum_trig;
    uint8_t sum = 0x5A;
    for (int i = 0; i < kStateBytes - 1; i++) sum = (uint8_t)(sum * 31u + out[i]);
    out[kStateBytes - 1] = sum;
}

bool Unpack(const uint8_t* in, MasterParams& m, VoiceParams v[kVoices], uint8_t& sel)
{
    uint8_t sum = 0x5A;
    for (int i = 0; i < kStateBytes - 1; i++) sum = (uint8_t)(sum * 31u + in[i]);
    if (in[0] != kLayoutVersion || sum != in[kStateBytes - 1]) return false;
    sel = (uint8_t)(in[1] % kVoices);
    for (int j = 0; j < kJacks; j++)
    {
        m.jack[j] = (uint8_t)(in[2 + j] % kNumJackFns);
        if (j >= kNumInJacks && IsCvIn(m.jack[j])) m.jack[j] = j == 6 ? kFnPitchA : kFnPitchB;   /* J9, J10: outputs only */
        if (j < kNumInJacks && IsAudio(m.jack[j])) m.jack[j] = kLayouts[kLayoutBand][j];        /* AUDIO: J9, J10 only */
    }
    m.trig_ms  = clampf((float)in[10], kTrigMsMin, kTrigMsMax);
    m.accent   = (uint8_t)clampi(in[11], 1, 127);
    m.bend     = (uint8_t)clampi(in[12], 0, kBendMax);
    m.reset_on = (uint8_t)(in[13] % kNumResetOn);
    m.clk_ppqn = (uint8_t)(in[14] % kNumPpqn);
    m.j2_mode  = (uint8_t)(in[15] % kNumJ2Modes);
    for (int i = 0; i < kVoices; i++)
    {
        m.cal_off[i]   = clampf((float)get16(&in[16 + 2 * i]) / 10000.0f, -kCalOffMax, kCalOffMax);
        m.cal_scale[i] = clampf((float)get16(&in[22 + 2 * i]) / 100000.0f, -kCalScaleMax, kCalScaleMax);
    }
    for (int i = 0; i < kVoices; i++)
    {
        const uint8_t* p = &in[28 + 10 * i];
        v[i].chan      = (uint8_t)(p[0] % kNumChans);
        v[i].gate_mode = (uint8_t)(p[1] % kNumGateModes);
        v[i].note_mode = (uint8_t)(p[2] % kNumNoteModes);
        v[i].glide     = (float)p[3] / 255.0f;
        v[i].octave    = (int8_t)clampi((int)p[4] - 8, kOctMin, kOctMax);
        v[i].transpose = (int8_t)clampi((int)p[5] - 16, kTransMin, kTransMax);
        v[i].att       = (float)p[6] / 255.0f;
        v[i].dec       = (float)p[7] / 255.0f;
        v[i].sus       = (float)p[8] / 255.0f;
        v[i].rel       = (float)p[9] / 255.0f;
    }
    m.drum_vel = in[58];
    m.drum_trig = in[67];
    for (int d = 0; d < kDrums; d++) m.drum_floor[d] = clampf((float)in[59 + d] / 50.0f, 0.0f, kDrumFloorMax);
    return true;
}

/* ---- engine -------------------------------------------------------------- */

void Engine::Init(float sample_rate)
{
    sr_ = sample_rate;
    Defaults(master, voice);
    for (int v = 0; v < kVoices; v++) vs_[v] = Voice{};
    for (int d = 0; d < kDrums; d++) drum_low_[d] = 1 << 20;
}

int32_t Engine::trig_samples() const
{
    return (int32_t)(clampf(master.trig_ms, kTrigMsMin, kTrigMsMax) * sr_ * 0.001f);
}

/* A trigger. Clock trigs are kept under half a tick, so 24 PPQN at a fast
 * tempo still comes out as separate pulses. */
void Engine::fire(int32_t* t, bool clock)
{
    int32_t n = trig_samples();
    if (clock && tick_len_ && (int32_t)(tick_len_ / 2u) < n) n = (int32_t)(tick_len_ / 2u);
    if (n < 1) n = 1;
    *t = n;
}

float Engine::note_volts(int v, int note) const
{
    const VoiceParams& P = voice[v];
    return (float)(note - kRootNote + P.transpose) / 12.0f + (float)P.octave;
}

/* The voice moves to `note`. fresh: its gate was down. */
void Engine::go(int v, int note, int vel, bool fresh)
{
    Voice&             V = vs_[v];
    const VoiceParams& P = voice[v];
    const bool         glide = P.glide > 0.004f;
    V.note   = note;
    V.vel    = (uint8_t)vel;
    V.target = note_volts(v, note);
    V.act    = 1.0f;
    if (fresh)
    {
        /* LEGATO glides only between overlapping notes (a slide); the other
         * modes glide into every note. The gate waits 1 ms for the pitch. */
        V.slide = glide && P.note_mode != kLegato;
        if (!V.slide) V.pitch = V.target;
        /* A note right after a note off (the same USB frame) still gets a
         * gap: the gate stays down at least 3 ms, as RETRIG's. */
        int32_t rise = (int32_t)(sr_ * 0.001f);
        const int32_t gap = (int32_t)(sr_ * 0.003f);
        if (V.fell && now_ - V.fell_at < (uint32_t)gap && gap - (int32_t)(now_ - V.fell_at) > rise)
            rise = gap - (int32_t)(now_ - V.fell_at);
        V.rise_in = rise;
        return;
    }
    V.slide = glide;
    if (!V.slide) V.pitch = V.target;
    if (P.note_mode == kRetrig)
    {
        /* Down for 3 ms (long enough for the 1 kHz I2C jacks to show it). */
        V.gate    = false;
        V.rise_in = (int32_t)(sr_ * 0.003f);
    }
}

void Engine::pick(int v, bool from_release)
{
    Voice&             V = vs_[v];
    const VoiceParams& P = voice[v];
    if (V.n == 0)
    {
        if (V.gate) { V.fell = true; V.fell_at = now_; }
        V.gate    = false;
        V.rise_in = -1;
        V.note    = -1;
        return;
    }
    int k = V.n - 1;   /* the newest */
    if (P.note_mode == kLow || P.note_mode == kHigh)
        for (int i = 0; i < V.n; i++)
            if (P.note_mode == kLow ? V.held[i] < V.held[k] : V.held[i] > V.held[k]) k = i;
    const bool active = V.gate || V.rise_in >= 0;
    if (active && V.held[k] == V.note && (from_release || P.note_mode == kLow || P.note_mode == kHigh))
        return;   /* the note sounding stays */
    go(v, V.held[k], V.hvel[k], !active);
}

void Engine::note_on(int v, int note, int vel)
{
    Voice& V = vs_[v];
    if (V.n >= kHeld)
    {
        for (int i = 1; i < V.n; i++) { V.held[i - 1] = V.held[i]; V.hvel[i - 1] = V.hvel[i]; }
        V.n--;
    }
    /* The same note twice is kept twice: PROTECTOR's slide to the same
     * note sends the new note on before the old one's off. */
    V.held[V.n] = (int8_t)note;
    V.hvel[V.n] = (uint8_t)vel;
    V.n++;
    pick(v, false);
}

void Engine::note_off(int v, int note)
{
    Voice& V = vs_[v];
    for (int i = 0; i < V.n; i++)
        if (V.held[i] == note)
        {
            for (int j = i + 1; j < V.n; j++) { V.held[j - 1] = V.held[j]; V.hvel[j - 1] = V.hvel[j]; }
            V.n--;
            pick(v, true);
            return;
        }
}

/* SysEx: gathered from its packets; a whole one that starts with TETHER's
 * ID (tether_sysex.h) is handed to the poll, one at a time. */
static bool sysex_ours(const uint8_t* p, int n) { return n >= 3 && p[0] == 0x7D && p[1] == 0x54 && p[2] == 0x45; }

void Engine::Packet(uint32_t pkt)
{
    const uint8_t cin = (uint8_t)(pkt & 0x0F);
    const int     nb  = cin == 0x4 || cin == 0x7 ? 3 : cin == 0x6 ? 2 : cin == 0x5 ? 1 : 0;
    if (nb && (cin == 0x4 || sx_n_ >= 0 || (uint8_t)(pkt >> 8) == 0xF0))
    {
        for (int i = 0; i < nb; i++)
        {
            const uint8_t b = (uint8_t)(pkt >> (8 * (i + 1)));
            if (b == 0xF0) { sx_n_ = 0; continue; }
            if (sx_n_ < 0) continue;
            if (b == 0xF7)
            {
                if (sysex_ours(sx_, sx_n_) && sx_done_n_.load(std::memory_order_acquire) == 0)
                {
                    memcpy(sx_done_, sx_, (size_t)sx_n_);
                    sx_done_n_.store(sx_n_, std::memory_order_release);
                }
                sx_n_ = -1;
                continue;
            }
            if (b & 0x80) { sx_n_ = -1; continue; }   /* broken off */
            if (sx_n_ < (int)sizeof sx_) sx_[sx_n_++] = b;
            else sx_n_ = -1;
        }
        return;
    }
    if (cin < 0x5 && cin != 0x2 && cin != 0x3) return;   /* 0/1 reserved, 4 SysEx */
    if (cin >= 0x5 && cin <= 0x7 && ((pkt >> 8) & 0xFF) < 0xF8) return;   /* SysEx ends */
    Midi((uint8_t)(pkt >> 8), (uint8_t)(pkt >> 16) & 0x7F, (uint8_t)(pkt >> 24) & 0x7F);
}

void Engine::Midi(uint8_t st, uint8_t d1, uint8_t d2)
{
    if (!(st & 0x80)) return;
    if (st >= 0xF0)
    {
        switch (st)
        {
            case 0xF8:
            {
                if (ticks_seen_ && now_ - last_tick_ < (uint32_t)sr_) tick_len_ = now_ - last_tick_;
                ticks_seen_ = true;
                last_tick_  = now_;
                if (ext_) { ext_ = false; ext_left_ = 0; }   /* MIDI clock wins over J1 */
                if (!running_ && transport_seen_) break;   /* stopped: the clock jacks rest */
                tick();
                break;
            }
            case 0xFA:
                transport_seen_ = true;
                start(now_);
                break;
            case 0xFB: transport_seen_ = running_ = true; break;
            case 0xFC:
                transport_seen_ = true;
                running_        = false;
                break;
            case 0xF2: next_tick_ = ((uint32_t)d1 | ((uint32_t)d2 << 7)) * 6u; break;
            default: break;
        }
        return;
    }
    const int ch = st & 0x0F, type = st & 0xF0;
    midi_count_++;
    midi_flash_ = 1.0f;
    if (type == 0x90 && d2 > 0)
    {
        if (ch < kDrums)
        {
            /* a gate from note on to note off, at least TRIG LENGTH (or,
             * set to TRIG, just TRIG LENGTH); its height the velocity's
             * when set. Still up from the last hit: down 3 ms first, so the
             * module sees a new one. */
            const int32_t gap = (int32_t)(sr_ * 0.003f);
            if (drum_[ch] > 0 || drum_held_[ch]) drum_gap_[ch] = gap;
            /* went down only just now (the last note's off in the same
             * moment): keep it down the rest of the 3 ms (the I2C jacks J3-J6
             * need that long to show it), or the jack never
             * drops and the module hears one long hit */
            else if (drum_low_[ch] < gap) drum_gap_[ch] = gap - drum_low_[ch];
            fire(&drum_[ch], false);
            drum_[ch] += drum_gap_[ch];
            drum_held_[ch] = !((master.drum_trig >> ch) & 1u);
            const float fl = clampf(master.drum_floor[ch], 0.0f, kDrumFloorMax);
            drum_v_[ch] = (master.drum_vel >> ch) & 1u ? fl + (kGateV - fl) * (float)(d2 - 1) / 126.0f : kGateV;
        }
        for (int v = 0; v < kVoices; v++)
            if (voice[v].chan == ch || voice[v].chan == kChanAll) note_on(v, d1, d2);
    }
    else if (type == 0x80 || type == 0x90)
    {
        if (ch < kDrums) drum_held_[ch] = false;
        for (int v = 0; v < kVoices; v++)
            if (voice[v].chan == ch || voice[v].chan == kChanAll) note_off(v, d1);
    }
    else if (type == 0xB0)
    {
        if (ch == 15 && d1 == 85)   /* PROTECTOR's section, A..D */
        {
            const uint8_t s = (uint8_t)(d2 & 3);
            if (s != section_ && master.reset_on == kResetStartSection) fire(&reset_, false);
            section_ = s;
        }
        else if (d1 == 120 || d1 == 123)   /* all sound / all notes off */
            for (int v = 0; v < kVoices; v++)
                if (voice[v].chan == ch || voice[v].chan == kChanAll) { vs_[v].n = 0; pick(v, true); }
    }
    else if (type == 0xE0)
    {
        const float b = (float)(((int)d1 | ((int)d2 << 7)) - 8192) / 8192.0f;
        for (int v = 0; v < kVoices; v++)
            if (voice[v].chan == ch || voice[v].chan == kChanAll) vs_[v].bend = b;
    }
}

bool Engine::TakeSysex(uint8_t* buf, int cap, int* len)
{
    const int n = sx_done_n_.load(std::memory_order_acquire);
    if (n <= 0) return false;
    const int k = n < cap ? n : cap;
    memcpy(buf, sx_done_, (size_t)k);
    *len = k;
    sx_done_n_.store(0, std::memory_order_release);
    return true;
}

void Engine::tick()
{
    const uint32_t t = next_tick_++;
    fire(&clk_[0], true);
    if (t % 6u == 0u) fire(&clk_[1], true);
    if (t % 12u == 0u) fire(&clk_[2], true);
    if (t % 24u == 0u) fire(&clk_[3], true);
    if (t % 96u == 0u) fire(&clk_[4], true);
}

void Engine::start(uint32_t t)
{
    running_   = true;
    next_tick_ = 0;
    ext_left_  = 0;
    fire(&reset_, false);
    started_  = true;
    start_at_ = t;
}

/* ---- J1 / J2 (0.3.0) ------------------------------------------------------------
 * J1: each pulse is 24 / PPQN MIDI clock steps. The first goes at the pulse;
 * the rest are spread over the time to the next pulse (as last measured), and
 * any still to come when the next pulse arrives go at once, so the count never
 * drifts. With J2 on RESET, a clock arriving starts the transport and the
 * clock stopping (three of its periods with nothing) stops it; with J2 on
 * START/STOP, J2 does that and J1 only counts while running. Everything here
 * waits while USB MIDI clock is coming in. */

void Engine::j1_edge(uint32_t t)
{
    const int per = 24 / kPpqnValues[master.clk_ppqn % kNumPpqn];
    if (!ext_)
    {
        ext_        = true;
        ext_period_ = 0;
        ext_left_   = 0;
        if (master.j2_mode == kJ2Reset && !running_) start(t);
    }
    else
    {
        const uint32_t iv = t - ext_last_;
        ext_period_ = ext_period_ ? (ext_period_ + iv) / 2u : iv;
    }
    ext_last_ = t;
    while (ext_left_ > 0) { ext_left_--; if (running_) tick(); }   /* the last pulse's, now */
    if (!running_) return;
    /* until a period is known: as if at 120 BPM */
    const uint32_t period = ext_period_ ? ext_period_ : (uint32_t)(sr_ * 0.5f / (float)(24 / per));
    ext_step_  = period / (uint32_t)per;
    tick_len_  = ext_step_;
    edge_tick_ = next_tick_;
    tick();
    ext_left_ = per - 1;
    ext_next_ = t + ext_step_;
}

void Engine::j2_edge(uint32_t t)
{
    const uint32_t near = (uint32_t)(sr_ * 0.002f);   /* 2 ms: the same moment */
    if (master.j2_mode == kJ2StartStop)
    {
        if (running_) { running_ = false; ext_left_ = 0; }
        else start(t);
        return;
    }
    if (started_ && t - start_at_ < near) return;    /* the start's own reset */
    if (ext_ && running_ && t - ext_last_ < near)
    {
        /* the clock pulse that just came is the downbeat */
        next_tick_ -= edge_tick_;
        fire(&reset_, false);
        started_ = true; start_at_ = t;
        return;
    }
    next_tick_ = 0;
    ext_left_  = 0;   /* the next J1 pulse is the downbeat */
    fire(&reset_, false);
    started_ = true; start_at_ = t;
}

void Engine::Inputs(const float* j1, const float* j2, uint32_t n)
{
    /* Thresholds as Pulse: 1.0 V up, 0.5 V down. */
    constexpr float kHi = 0.2f, kLo = 0.1f;
    if (MidiClock())
    {
        /* MIDI wins: keep the edge state, act on nothing */
        if (j1 && n) j1_hi_ = j1[n - 1] > kLo && (j1_hi_ || j1[n - 1] > kHi);
        if (j2 && n) j2_hi_ = j2[n - 1] > kLo && (j2_hi_ || j2[n - 1] > kHi);
        return;
    }
    for (uint32_t i = 0; i < n; i++)
    {
        bool e2 = false, e1 = false;
        if (j2)
        {
            const float v = j2[i];
            if (!j2_hi_ && v > kHi) { j2_hi_ = true; e2 = true; }
            else if (j2_hi_ && v < kLo) j2_hi_ = false;
        }
        if (j1)
        {
            const float v = j1[i];
            if (!j1_hi_ && v > kHi) { j1_hi_ = true; e1 = true; }
            else if (j1_hi_ && v < kLo) j1_hi_ = false;
        }
        if (e2) j2_edge(now_ + i);   /* a reset with a pulse: reset first, the pulse is beat one */
        if (e1) j1_edge(now_ + i);
    }
}

void Engine::Process(uint32_t n)
{
    /* J1: the rest of the last pulse's steps; the clock gone quiet */
    if (ext_)
    {
        while (ext_left_ > 0 && (int32_t)(now_ - ext_next_) >= 0)
        {
            ext_left_--;
            ext_next_ += ext_step_;
            if (running_) tick();
        }
        float lim = ext_period_ ? 3.0f * (float)ext_period_ / sr_ : 2.0f;
        if (lim < 0.25f) lim = 0.25f;
        if (lim > 4.0f) lim = 4.0f;
        if ((int32_t)(now_ - ext_last_) > (int32_t)(lim * sr_))   /* the pulse may be later in this block */
        {
            ext_      = false;
            ext_left_ = 0;
            if (master.j2_mode == kJ2Reset) running_ = false;
        }
    }
    const int32_t dn = (int32_t)n;
    for (int i = 0; i < 5; i++) clk_[i] = clk_[i] > dn ? clk_[i] - dn : 0;
    reset_ = reset_ > dn ? reset_ - dn : 0;
    for (int i = 0; i < kDrums; i++)
    {
        drum_[i]     = drum_[i] > dn ? drum_[i] - dn : 0;
        drum_gap_[i] = drum_gap_[i] > dn ? drum_gap_[i] - dn : 0;
        if (drum_gap_[i] > 0 || (drum_[i] == 0 && !drum_held_[i])) drum_low_[i] = drum_low_[i] + (int32_t)dn < (1 << 20) ? drum_low_[i] + (int32_t)dn : 1 << 20;
        else drum_low_[i] = 0;
    }
    const float decay = (float)n / (0.25f * sr_);
    for (int v = 0; v < kVoices; v++)
    {
        Voice& V = vs_[v];
        V.trig_left = V.trig_left > dn ? V.trig_left - dn : 0;
        if (V.rise_in >= 0)
        {
            V.rise_in -= dn;
            if (V.rise_in <= 0)
            {
                V.rise_in   = -1;
                V.gate      = true;
                V.trig_left = trig_samples();
            }
        }
        if (V.slide)
        {
            const float tau = 0.001f + voice[v].glide * voice[v].glide;   /* s */
            const float k   = 1.0f - expf(-(float)n / (tau * sr_));
            V.pitch += (V.target - V.pitch) * k;
            if (fabsf(V.target - V.pitch) < 1e-5f) V.pitch = V.target;
        }
        else
            V.pitch = V.target;
        V.act = V.act > decay ? V.act - decay : 0.0f;

        /* the envelope: attack on the gate going up (a new note after a
         * gap, or RETRIG's), decay to sustain, release when it goes down */
        const VoiceParams& P  = voice[v];
        const float        dt = (float)n / sr_;
        if (V.gate && !V.env_gate) V.env_stage = 1;
        if (!V.gate && V.env_gate) V.env_stage = 0;
        V.env_gate = V.gate;
        if (V.env_stage == 1)
        {
            V.env += dt / EnvSeconds(P.att);
            if (V.env >= 1.0f) { V.env = 1.0f; V.env_stage = 2; }
        }
        else
        {
            /* most of the way (98 %) in the knob's time */
            const float tgt = V.env_stage == 2 ? clampf(P.sus, 0.0f, 1.0f) : 0.0f;
            const float tau = 0.25f * EnvSeconds(V.env_stage == 2 ? P.dec : P.rel);
            V.env += (tgt - V.env) * (1.0f - expf(-dt / tau));
            if (V.env < 1e-4f && V.env_stage == 0) V.env = 0.0f;
        }
    }
    midi_flash_ = midi_flash_ > decay * 4.0f ? midi_flash_ - decay * 4.0f : 0.0f;
    now_ += n;
}

float Engine::PitchVolts(int v) const
{
    const Voice& V = vs_[v];
    float x = ref_ ? (ref_ == 1 ? 1.0f : 3.0f)
                   : V.pitch + V.bend * (float)master.bend / 12.0f;
    x = 1.0f + (x - 1.0f) * (1.0f + master.cal_scale[v]) + master.cal_off[v];
    return clampf(x, -5.0f, 5.0f);
}

float Engine::FnVolts(uint8_t fn) const
{
    if (fn <= kFnPitchC) return PitchVolts(fn - kFnPitchA);
    if (fn >= kFnEnvA && fn <= kFnEnvC) return ref_ ? kGateV : EnvVolts(fn - kFnEnvA);
    if (fn >= kFnGateA && fn <= kFnGateC)
    {
        const int    v = fn - kFnGateA;
        const Voice& V = vs_[v];
        if (ref_) return kGateV;   /* calibrating: a note you can hear */
        switch (voice[v].gate_mode)
        {
            case kTrig: return V.trig_left > 0 ? kGateV : 0.0f;
            case kAccGate: return V.gate ? (V.vel >= master.accent ? kAccentV : kPlainAccV) : 0.0f;
            case kVelGate: return V.gate ? 1.0f + 4.0f * (float)V.vel / 127.0f : 0.0f;
            default: return V.gate ? kGateV : 0.0f;
        }
    }
    if (fn >= kFnClk24 && fn <= kFnBar) return clk_[fn - kFnClk24] > 0 ? kGateV : 0.0f;
    switch (fn)
    {
        case kFnReset: return reset_ > 0 ? kGateV : 0.0f;
        case kFnRun: return running_ ? kGateV : 0.0f;
        case kFnSection: return (float)section_;
        case kFnAccA:
        case kFnAccB:
        case kFnAccC:
        {
            const Voice& V = vs_[fn - kFnAccA];
            return V.gate && V.vel >= master.accent ? kGateV : 0.0f;
        }
        case kFnVelA:
        case kFnVelB:
        case kFnVelC: return kGateV * (float)vs_[fn - kFnVelA].vel / 127.0f;
        default: break;
    }
    if (fn >= kFnTrig1 && fn < kFnTrig1 + kDrums)
    {
        const int d = fn - kFnTrig1;
        return drum_gap_[d] == 0 && (drum_[d] > 0 || drum_held_[d]) ? drum_v_[d] : 0.0f;
    }
    return 0.0f;
}

float Engine::JackVolts(int j) const { return j >= 0 && j < kJacks ? FnVolts(master.jack[j]) : 0.0f; }

void Engine::Panic()
{
    for (int v = 0; v < kVoices; v++)
    {
        vs_[v].n = 0;
        vs_[v].bend = 0.0f;
        pick(v, true);
        vs_[v].trig_left = 0;
    }
    for (int i = 0; i < 5; i++) clk_[i] = 0;
    for (int i = 0; i < kDrums; i++) { drum_[i] = drum_gap_[i] = 0; drum_low_[i] = 1 << 20; drum_held_[i] = false; }
    reset_ = 0;
}

void Engine::Test(int v, bool on)
{
    if (v < 0 || v >= kVoices) return;
    if (on) note_on(v, kRootNote, 100);
    else note_off(v, kRootNote);
}

} // namespace tether
