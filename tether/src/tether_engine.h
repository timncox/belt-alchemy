/*
 * tether_engine -- TETHER's MIDI-to-CV core, with no SDK in it (natively
 * tested in ../test). The firmware feeds it the USB-MIDI packets the Lab's
 * USB host receives and runs it once per audio block; it says what each
 * jack should put out.
 *
 *   Two voices, A and B. Each follows one MIDI channel (or all of them):
 *   pitch (1 V/oct) on J9 / J10, a gate wherever a jack is set to GATE A /
 *   GATE B. J3-J8 each pick a job from JackFn: gates, MIDI clock at four
 *   rates, bar, reset, run, PROTECTOR's section as a voltage, accents,
 *   velocities, and a trigger per drum channel.
 *
 * Time is in samples; Process(n) is one block. Packets go in from the audio
 * thread (the firmware drains a Ring the USB poll fills).
 */
#pragma once
#include <stdint.h>

#include <atomic>

namespace tether
{

constexpr int kVoices   = 2;
constexpr int kJacks    = 6;    /* J3..J8 */
constexpr int kChanAll  = 16;   /* a voice's channel: 0..15, or 16 = every channel */
constexpr int kNumChans = 17;
constexpr int kHeld     = 16;   /* notes a voice remembers */

enum GateMode : uint8_t { kGate, kTrig, kAccGate, kVelGate, kNumGateModes };
enum NoteMode : uint8_t { kLegato, kRetrig, kLow, kHigh, kNumNoteModes };
enum ResetOn  : uint8_t { kResetStart, kResetStartSection, kNumResetOn };

enum JackFn : uint8_t
{
    kFnGateA, kFnGateB,
    kFnClk24, kFnClk16, kFnClk8, kFnClk4, kFnBar,   /* 24 PPQN, 16ths, 8ths, quarters, bars */
    kFnReset, kFnRun, kFnSection,
    kFnAccA, kFnAccB, kFnVelA, kFnVelB,
    kFnTrig1,                                       /* + channel 0..7: KICK..FX in PROTECTOR */
    kNumJackFns = kFnTrig1 + 8
};

constexpr int   kOctMin = -4, kOctMax = 4;
constexpr int   kTransMin = -12, kTransMax = 12;
constexpr int   kBendMax = 12;
constexpr float kTrigMsMin = 1.0f, kTrigMsMax = 50.0f;
constexpr float kCalOffMax = 0.1f;     /* V */
constexpr float kCalScaleMax = 0.05f;  /* +-5 % */
constexpr float kGateV = 5.0f, kAccentV = 8.0f;
constexpr int   kRootNote = 60;        /* the note that puts out 0 V (before OCTAVE) */

struct VoiceParams
{
    uint8_t chan;        /* 0..15, kChanAll */
    uint8_t gate_mode;   /* GateMode */
    uint8_t note_mode;   /* NoteMode */
    float   glide;       /* 0..1 -> 0..1 s */
    int8_t  octave;      /* kOctMin..kOctMax */
    int8_t  transpose;   /* kTransMin..kTransMax semitones */
};

struct MasterParams
{
    uint8_t jack[kJacks];          /* JackFn of J3..J8 */
    float   trig_ms;
    uint8_t accent;                /* velocity at or above which a note is accented */
    uint8_t bend;                  /* pitch-bend range, semitones */
    uint8_t reset_on;              /* ResetOn */
    float   cal_off[kVoices];      /* V */
    float   cal_scale[kVoices];    /* fraction */
};

void Defaults(MasterParams& m, VoiceParams v[kVoices]);

constexpr int     kStateBytes   = 48;
constexpr uint8_t kLayoutVersion = 1;
void Pack(const MasterParams& m, const VoiceParams v[kVoices], uint8_t sel, uint8_t* out);
bool Unpack(const uint8_t* in, MasterParams& m, VoiceParams v[kVoices], uint8_t& sel);

const char* JackName(uint8_t fn);   /* "GATE A", "CLK 24", "TRIG 1"... */

/* A single-producer single-consumer queue of 32-bit USB-MIDI packets:
 * byte 0 cable|CIN, byte 1 status, byte 2 data 1, byte 3 data 2. */
template <int N>
struct Ring
{
    std::atomic<uint32_t> w{0}, r{0};
    uint32_t              buf[N];
    uint32_t              dropped = 0;
    bool Push(uint32_t x)
    {
        const uint32_t wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= (uint32_t)N) { dropped++; return false; }
        buf[wi % N] = x;
        w.store(wi + 1u, std::memory_order_release);
        return true;
    }
    bool Pop(uint32_t* x)
    {
        const uint32_t ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        *x = buf[ri % N];
        r.store(ri + 1u, std::memory_order_release);
        return true;
    }
    void Clear() { r.store(w.load(std::memory_order_acquire), std::memory_order_release); }
};

class Engine
{
  public:
    MasterParams master;
    VoiceParams  voice[kVoices];

    void Init(float sample_rate);

    void Packet(uint32_t pkt);                       /* one USB-MIDI event packet */
    void Midi(uint8_t status, uint8_t d1, uint8_t d2);
    void Process(uint32_t frames);

    float PitchVolts(int v) const;     /* J9 (A), J10 (B): calibrated, +-5 V */
    float JackVolts(int j) const;      /* J3..J8 (j 0..5) */
    float FnVolts(uint8_t fn) const;

    bool    Gate(int v) const { return vs_[v].gate; }
    int     Note(int v) const { return vs_[v].note; }
    float   Activity(int v) const { return vs_[v].act; }
    bool    Running() const { return running_; }
    bool    ClockSeen() const { return now_ - last_tick_ < (uint32_t)(sr_ * 0.5f) && ticks_seen_; }
    float   BeatPhase() const { return (float)((next_tick_ + 23) % 24) / 24.0f; }
    uint8_t Section() const { return section_; }
    uint32_t MidiCount() const { return midi_count_; }
    float   MidiFlash() const { return midi_flash_; }

    void Panic();
    void Test(int v, bool on);         /* B3: a C on the voice, whatever its channel */
    void SetRef(int r) { ref_ = r; }   /* calibration: 0 off, 1 -> 1 V, 2 -> 3 V on J9/J10 */
    int  Ref() const { return ref_; }

  private:
    struct Voice
    {
        int8_t   held[kHeld];
        uint8_t  hvel[kHeld];
        int      n = 0;
        int      note = -1;           /* sounding (or about to) */
        uint8_t  vel = 0;
        bool     gate = false;
        int32_t  rise_in = -1;        /* samples until the gate goes up, -1 none */
        int32_t  trig_left = 0;       /* TRIG mode / accent pulse */
        float    pitch = 0.0f, target = 0.0f;   /* V, before calibration */
        bool     slide = false;       /* glide toward target */
        float    bend = 0.0f;         /* -1..1 */
        float    act = 0.0f;
        bool     fell = false;        /* the gate went down at fell_at (samples) */
        uint32_t fell_at = 0;
    };

    void  note_on(int v, int note, int vel);
    void  note_off(int v, int note);
    void  pick(int v, bool from_release);
    void  go(int v, int note, int vel, bool fresh);
    float note_volts(int v, int note) const;
    void  fire(int32_t* t, bool clock);
    int32_t trig_samples() const;

    float    sr_ = 48000.0f;
    uint32_t now_ = 0;
    Voice    vs_[kVoices];
    bool     running_ = false, transport_seen_ = false, ticks_seen_ = false;
    uint32_t next_tick_ = 0, last_tick_ = 0, tick_len_ = 0;
    int32_t  clk_[5] = {0, 0, 0, 0, 0};       /* 24, 16, 8, 4, bar */
    int32_t  reset_ = 0, drum_[8] = {0};
    uint8_t  section_ = 0;
    int      ref_ = 0;
    uint32_t midi_count_ = 0;
    float    midi_flash_ = 0.0f;
};

} // namespace tether
