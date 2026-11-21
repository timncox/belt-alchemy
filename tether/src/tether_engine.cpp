/* tether_engine -- see tether_engine.h. */
#include "tether_engine.h"

#include <math.h>
#include <string.h>

namespace tether
{

void Defaults(MasterParams& m, VoiceParams v[kVoices])
{
    /* J3 J4 gates, J5 clock (16ths), J6 reset, J7 J8 KICK and SNARE. */
    const uint8_t jacks[kJacks] = {kFnGateA, kFnGateB, kFnClk16, kFnReset, kFnTrig1, kFnTrig1 + 1};
    memcpy(m.jack, jacks, sizeof jacks);
    m.trig_ms  = 5.0f;
    m.accent   = 110;     /* PROTECTOR's accented steps */
    m.bend     = 2;
    m.reset_on = kResetStart;
    for (int i = 0; i < kVoices; i++) { m.cal_off[i] = 0.0f; m.cal_scale[i] = 0.0f; }
    /* A: PROTECTOR's BASS (channel 5), B: its LEAD (6). */
    for (int i = 0; i < kVoices; i++)
    {
        v[i].chan      = (uint8_t)(4 + i);
        v[i].gate_mode = kGate;
        v[i].note_mode = kLegato;
        v[i].glide     = 0.0f;
        v[i].octave    = 0;
        v[i].transpose = 0;
    }
}

static const char* const kJackNames[kNumJackFns] = {
    "GATE A", "GATE B", "CLK 24", "CLK 16", "CLK 8", "CLK 4", "BAR", "RESET", "RUN", "SECTION",
    "ACC A", "ACC B", "VEL A", "VEL B",
    "TRIG 1", "TRIG 2", "TRIG 3", "TRIG 4", "TRIG 5", "TRIG 6", "TRIG 7", "TRIG 8"};

const char* JackName(uint8_t fn) { return fn < kNumJackFns ? kJackNames[fn] : "?"; }

/* ---- state ---------------------------------------------------------------- */

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static void put16(uint8_t* p, int v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
static int  get16(const uint8_t* p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

void Pack(const MasterParams& m, const VoiceParams v[kVoices], uint8_t sel, uint8_t* out)
{
    memset(out, 0, kStateBytes);
    out[0] = kLayoutVersion;
    out[1] = sel;
    for (int j = 0; j < kJacks; j++) out[2 + j] = m.jack[j];
    out[8]  = (uint8_t)lroundf(m.trig_ms);
    out[9]  = m.accent;
    out[10] = m.bend;
    out[11] = m.reset_on;
    for (int i = 0; i < kVoices; i++)
    {
        put16(&out[12 + 2 * i], (int)lroundf(m.cal_off[i] * 10000.0f));     /* 0.1 mV */
        put16(&out[16 + 2 * i], (int)lroundf(m.cal_scale[i] * 100000.0f));  /* 10 ppm */
    }
    for (int i = 0; i < kVoices; i++)
    {
        uint8_t* p = &out[20 + 6 * i];
        p[0] = v[i].chan;
        p[1] = v[i].gate_mode;
        p[2] = v[i].note_mode;
        p[3] = (uint8_t)lroundf(clampf(v[i].glide, 0.0f, 1.0f) * 255.0f);
        p[4] = (uint8_t)(v[i].octave + 8);
        p[5] = (uint8_t)(v[i].transpose + 16);
    }
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
    for (int j = 0; j < kJacks; j++) m.jack[j] = (uint8_t)(in[2 + j] % kNumJackFns);
    m.trig_ms  = clampf((float)in[8], kTrigMsMin, kTrigMsMax);
    m.accent   = (uint8_t)clampi(in[9], 1, 127);
    m.bend     = (uint8_t)clampi(in[10], 0, kBendMax);
    m.reset_on = (uint8_t)(in[11] % kNumResetOn);
    for (int i = 0; i < kVoices; i++)
    {
        m.cal_off[i]   = clampf((float)get16(&in[12 + 2 * i]) / 10000.0f, -kCalOffMax, kCalOffMax);
        m.cal_scale[i] = clampf((float)get16(&in[16 + 2 * i]) / 100000.0f, -kCalScaleMax, kCalScaleMax);
    }
    for (int i = 0; i < kVoices; i++)
    {
        const uint8_t* p = &in[20 + 6 * i];
        v[i].chan      = (uint8_t)(p[0] % kNumChans);
        v[i].gate_mode = (uint8_t)(p[1] % kNumGateModes);
        v[i].note_mode = (uint8_t)(p[2] % kNumNoteModes);
        v[i].glide     = (float)p[3] / 255.0f;
        v[i].octave    = (int8_t)clampi((int)p[4] - 8, kOctMin, kOctMax);
        v[i].transpose = (int8_t)clampi((int)p[5] - 16, kTransMin, kTransMax);
    }
    return true;
}

/* ---- engine -------------------------------------------------------------- */

void Engine::Init(float sample_rate)
{
    sr_ = sample_rate;
    Defaults(master, voice);
    for (int v = 0; v < kVoices; v++) vs_[v] = Voice{};
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

void Engine::Packet(uint32_t pkt)
{
    const uint8_t cin = (uint8_t)(pkt & 0x0F);
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
                if (!running_ && transport_seen_) break;   /* stopped: the clock jacks rest */
                const uint32_t t = next_tick_++;
                fire(&clk_[0], true);
                if (t % 6u == 0u) fire(&clk_[1], true);
                if (t % 12u == 0u) fire(&clk_[2], true);
                if (t % 24u == 0u) fire(&clk_[3], true);
                if (t % 96u == 0u) fire(&clk_[4], true);
                break;
            }
            case 0xFA:
                transport_seen_ = running_ = true;
                next_tick_ = 0;
                fire(&reset_, false);
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
        if (ch < 8) fire(&drum_[ch], false);
        for (int v = 0; v < kVoices; v++)
            if (voice[v].chan == ch || voice[v].chan == kChanAll) note_on(v, d1, d2);
    }
    else if (type == 0x80 || type == 0x90)
    {
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

void Engine::Process(uint32_t n)
{
    const int32_t dn = (int32_t)n;
    for (int i = 0; i < 5; i++) clk_[i] = clk_[i] > dn ? clk_[i] - dn : 0;
    reset_ = reset_ > dn ? reset_ - dn : 0;
    for (int i = 0; i < 8; i++) drum_[i] = drum_[i] > dn ? drum_[i] - dn : 0;
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
    if (fn == kFnGateA || fn == kFnGateB)
    {
        const int    v = fn - kFnGateA;
        const Voice& V = vs_[v];
        if (ref_) return kGateV;   /* calibrating: a note you can hear */
        switch (voice[v].gate_mode)
        {
            case kTrig: return V.trig_left > 0 ? kGateV : 0.0f;
            case kAccGate: return V.gate ? (V.vel >= master.accent ? kAccentV : kGateV) : 0.0f;
            case kVelGate: return V.gate ? 1.0f + 7.0f * (float)V.vel / 127.0f : 0.0f;
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
        {
            const Voice& V = vs_[fn - kFnAccA];
            return V.gate && V.vel >= master.accent ? kGateV : 0.0f;
        }
        case kFnVelA:
        case kFnVelB: return 8.0f * (float)vs_[fn - kFnVelA].vel / 127.0f;
        default: break;
    }
    if (fn >= kFnTrig1 && fn < kNumJackFns) return drum_[fn - kFnTrig1] > 0 ? kGateV : 0.0f;
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
    for (int i = 0; i < 8; i++) drum_[i] = 0;
    reset_ = 0;
}

void Engine::Test(int v, bool on)
{
    if (v < 0 || v >= kVoices) return;
    if (on) note_on(v, kRootNote, 100);
    else note_off(v, kRootNote);
}

} // namespace tether
