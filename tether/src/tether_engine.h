/*
 * tether_engine -- TETHER's MIDI-to-CV core, with no SDK in it (natively
 * tested in ../test). The firmware feeds it the USB-MIDI packets the Lab's
 * USB host receives and runs it once per audio block; it says what each
 * jack should put out.
 *
 *   Three voices, A B C (PROTECTOR's BASS, LEAD and PAD at first). Each
 *   follows one MIDI channel (or all of them). Every output, J3-J10, picks a
 *   job from JackFn: a voice's pitch (1 V/oct), gate, envelope, accent or
 *   velocity, MIDI clock at four rates, bar, reset, run, PROTECTOR's section
 *   as a voltage, or a trigger per drum channel. Layouts set all eight at
 *   once: BAND (drums, bass, lead, clock out; the default from 0.3.0) and CLOCK
 *   (0.2.0's: two voices, clock and reset).
 *   J1 and J2, the inputs, take a clock and a reset (or start/stop) for
 *   when no MIDI clock is coming in (0.3.0).
 *
 * Time is in samples; Process(n) is one block. Packets go in from the audio
 * thread (the firmware drains a Ring the USB poll fills).
 */
#pragma once
#include <stdint.h>

#include <atomic>

namespace tether
{

constexpr int kVoices   = 3;
constexpr int kJacks    = 8;    /* J3..J10 */
constexpr int kChanAll  = 16;   /* a voice's channel: 0..15, or 16 = every channel */
constexpr int kNumChans = 17;
constexpr int kHeld     = 16;   /* notes a voice remembers */

enum GateMode : uint8_t { kGate, kTrig, kAccGate, kVelGate, kNumGateModes };
enum NoteMode : uint8_t { kLegato, kRetrig, kLow, kHigh, kNumNoteModes };
enum ResetOn  : uint8_t { kResetStart, kResetStartSection, kNumResetOn };
/* J2 (0.3.0): a pulse resets to the top of the bar, or starts and stops. */
enum J2Mode   : uint8_t { kJ2Reset, kJ2StartStop, kNumJ2Modes };

/* J1's clock (0.3.0): pulses per quarter note. */
constexpr int     kNumPpqn = 7;
constexpr uint8_t kPpqnValues[kNumPpqn] = {1, 2, 4, 8, 12, 24, 48};
constexpr uint8_t kPpqnDefault = 2;    /* 4 PPQN: a pulse every sixteenth */

enum JackFn : uint8_t
{
    kFnPitchA, kFnPitchB, kFnPitchC,                /* 1 V/oct, calibrated */
    kFnGateA, kFnGateB, kFnGateC,
    kFnEnvA, kFnEnvB, kFnEnvC,                      /* ADSR, 0-5 V, from the voice's gate */
    kFnAccA, kFnAccB, kFnAccC,
    kFnVelA, kFnVelB, kFnVelC,
    kFnClk24, kFnClk16, kFnClk8, kFnClk4, kFnBar,   /* 24 PPQN, 16ths, 8ths, quarters, bars */
    kFnReset, kFnRun, kFnSection,
    kFnTrig1,                                       /* + channel 0..7: KICK..FX in PROTECTOR */
    kNumJackFns = kFnTrig1 + 8
};

/* Whole layouts of J3..J10. */
enum Layout : uint8_t { kLayoutBand, kLayoutClock, kNumLayouts };
void        SetLayout(uint8_t jack[kJacks], int layout);
int         LayoutOf(const uint8_t jack[kJacks]);   /* -1: changed by hand */
const char* LayoutName(int layout);                 /* "BAND", "CLOCK", "CUSTOM" */

constexpr int   kOctMin = -4, kOctMax = 4;
constexpr int   kTransMin = -12, kTransMax = 12;
constexpr int   kBendMax = 12;
constexpr float kTrigMsMin = 1.0f, kTrigMsMax = 50.0f;
constexpr float kCalOffMax = 0.1f;     /* V */
constexpr float kCalScaleMax = 0.05f;  /* +-5 % */
/* Every output tops out at about 5 V (the DACs' and the codec's full scale,
 * SDK cv_jack.cpp), so 0.3.0 keeps everything inside it: an accent is 5 V
 * over a 3.5 V gate (0.2.0 asked for 8 V and got 5 either way). */
constexpr float kGateV = 5.0f, kAccentV = 5.0f, kPlainAccV = 3.5f;
constexpr int   kDrums = 8;           /* TRIG 1..8: MIDI channels 1..8 */
constexpr float kDrumFloorMax = 5.0f; /* V */
constexpr int   kRootNote = 60;        /* the note that puts out 0 V (before OCTAVE) */

struct VoiceParams
{
    uint8_t chan;        /* 0..15, kChanAll */
    uint8_t gate_mode;   /* GateMode */
    uint8_t note_mode;   /* NoteMode */
    float   glide;       /* 0..1 -> 0..1 s */
    int8_t  octave;      /* kOctMin..kOctMax */
    int8_t  transpose;   /* kTransMin..kTransMax semitones */
    float   att, dec, sus, rel;   /* envelope: times 0..1 (-> 1 ms..4 s), sustain level 0..1 */
};

float EnvSeconds(float x);       /* an envelope time knob, 0..1, in seconds */

struct MasterParams
{
    uint8_t jack[kJacks];          /* JackFn of J3..J10 */
    float   trig_ms;
    uint8_t accent;                /* velocity at or above which a note is accented */
    uint8_t bend;                  /* pitch-bend range, semitones */
    uint8_t reset_on;              /* ResetOn */
    float   cal_off[kVoices];      /* V */
    float   cal_scale[kVoices];    /* fraction */
    uint8_t drum_vel;              /* bit d: TRIG d+1's height follows velocity */
    float   drum_floor[kDrums];    /* V at velocity 1 (127 is 5 V), so soft hits still fire */
    uint8_t drum_trig;             /* bit d: TRIG d+1 is a fixed TRIG LENGTH pulse, not held to the note off */
    uint8_t clk_ppqn;              /* J1: index into kPpqnValues */
    uint8_t j2_mode;               /* J2Mode */
};

void Defaults(MasterParams& m, VoiceParams v[kVoices]);

/* 2 (0.3.0): three voices, eight jacks, envelopes, J1 / J2. Settings saved
 * by 0.1 / 0.2 (version 1, 48 bytes) don't load: TETHER starts from its
 * defaults. */
constexpr int     kStateBytes   = 80;
constexpr uint8_t kLayoutVersion = 2;
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
    /* J1 / J2, the codec inputs (5 V = 1.0), one block of samples before
     * Process. Rising edges only (the inputs are AC-coupled): J1 a clock,
     * J2 a reset or start/stop. Ignored while USB MIDI clock is coming in:
     * MIDI wins. Either may be null. */
    void Inputs(const float* j1, const float* j2, uint32_t frames);

    float PitchVolts(int v) const;     /* voice v's pitch: calibrated, +-5 V */
    float EnvVolts(int v) const { return kGateV * vs_[v].env; }
    float JackVolts(int j) const;      /* J3..J10 (j 0..7) */
    float FnVolts(uint8_t fn) const;

    bool    Gate(int v) const { return vs_[v].gate; }
    int     Note(int v) const { return vs_[v].note; }
    float   Activity(int v) const { return vs_[v].act; }
    bool    Running() const { return running_; }
    bool    ClockSeen() const { return MidiClock() || ext_; }    /* MIDI or J1 */
    bool    MidiClock() const { return now_ - last_tick_ < (uint32_t)(sr_ * 0.5f) && ticks_seen_; }
    bool    J1Clock() const { return ext_; }
    float   BeatPhase() const { return (float)((next_tick_ + 23) % 24) / 24.0f; }
    uint8_t Section() const { return section_; }
    uint32_t MidiCount() const { return midi_count_; }
    float   MidiFlash() const { return midi_flash_; }

    void Panic();
    void Test(int v, bool on);         /* B3: a C on the voice, whatever its channel */
    void SetRef(int r) { ref_ = r; }   /* calibration: 0 off, 1 -> 1 V, 2 -> 3 V on every pitch jack */

    /* Settings over SysEx (0.3.0; Loadout): a complete request that came in,
     * taken once by the firmware's poll, which answers it (tether_sysex.h). */
    bool TakeSysex(uint8_t* buf, int cap, int* len);
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
        float    env = 0.0f;          /* 0..1 */
        uint8_t  env_stage = 0;       /* 0 idle/release, 1 attack, 2 decay/sustain */
        bool     env_gate = false;
        bool     fell = false;        /* the gate went down at fell_at (samples) */
        uint32_t fell_at = 0;
    };

    void  note_on(int v, int note, int vel);
    void  note_off(int v, int note);
    void  pick(int v, bool from_release);
    void  go(int v, int note, int vel, bool fresh);
    float note_volts(int v, int note) const;
    void  fire(int32_t* t, bool clock);
    void  tick();                       /* one 24 PPQN clock step */
    void  start(uint32_t t);            /* from the top, with RESET */
    void  j1_edge(uint32_t t);
    void  j2_edge(uint32_t t);
    int32_t trig_samples() const;

    float    sr_ = 48000.0f;
    uint32_t now_ = 0;
    Voice    vs_[kVoices];
    bool     running_ = false, transport_seen_ = false, ticks_seen_ = false;
    uint32_t next_tick_ = 0, last_tick_ = 0, tick_len_ = 0;
    int32_t  clk_[5] = {0, 0, 0, 0, 0};       /* 24, 16, 8, 4, bar */
    int32_t  reset_ = 0, drum_[kDrums] = {0};   /* samples of trig left (the least it stays up) */
    bool     drum_held_[kDrums] = {};           /* note still on: a gate */
    int32_t  drum_gap_[kDrums] = {};            /* down this long first (a hit while still up) */
    int32_t  drum_low_[kDrums] = {};            /* samples since it last went down (capped) */
    float    drum_v_[kDrums] = {};              /* its height */
    uint8_t  section_ = 0;
    int      ref_ = 0;
    uint32_t midi_count_ = 0;
    /* J1 / J2 */
    bool     j1_hi_ = false, j2_hi_ = false;
    bool     ext_ = false;              /* J1 is driving the clock */
    uint32_t ext_last_ = 0, ext_period_ = 0;
    int      ext_left_ = 0;             /* 24 PPQN steps still to come from the last J1 pulse */
    uint32_t ext_step_ = 0, ext_next_ = 0;
    uint32_t edge_tick_ = 0;            /* next_tick_ just before the last J1 pulse's own step */
    bool     started_ = false;
    uint32_t start_at_ = 0;
    /* SysEx being received (audio thread) and one finished request (poll) */
    uint8_t  sx_[200];
    int      sx_n_ = -1;                /* -1: not in a SysEx */
    uint8_t  sx_done_[200];
    struct Count   /* an atomic int that copies (the tests copy engines) */
    {
        std::atomic<int> v{0};
        Count() = default;
        Count(const Count& o) : v(o.v.load()) {}
        Count& operator=(const Count& o) { v.store(o.v.load()); return *this; }
        int  load(std::memory_order m) const { return v.load(m); }
        void store(int x, std::memory_order m) { v.store(x, m); }
    } sx_done_n_;
    float    midi_flash_ = 0.0f;
};

} // namespace tether
