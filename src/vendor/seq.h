/*
 * seq -- the sequencer, the part that has no hardware in it.
 *
 * Two generated voices (SEQ_BASS, SEQ_LEAD), each a pitch in volts plus a
 * gate, and four programmed drum triggers (kick, snare, hat, perc). The
 * board calls seq_process() about once a millisecond with the time in
 * microseconds and copies seq_out_t to the jacks; clock and reset edges come
 * in through seq_clock_edge() / seq_reset_edge(). Everything runs in one
 * thread. No allocation.
 *
 * GENERATION (the Vector idea). A voice's pattern is not a list of notes but
 * one 32-bit seed per step. Every property of a step is a fixed hash of that
 * seed -- does it play, how long, which chord tone, slide, octave jump -- and
 * the knobs are THRESHOLDS against those fixed numbers, never re-rolls. So:
 *   - raising DENSITY only ever adds notes, and lowering it then raising it
 *     again brings back exactly the same line;
 *   - raising LENGTH only ever lengthens notes;
 *   - the same seeds always make the same line.
 * ROLL replaces every step seed, MUTATE replaces a fraction of them, UNDO
 * swaps the previous seeds back (one level, and UNDO again is redo).
 *
 * CHORDS. Pitches are stored as an index into the CURRENT chord (or scale),
 * not as notes, so when the progression moves to the next chord the same
 * line re-voices to fit it. LOCK freezes the realized steps (rhythm, length,
 * slide, tone index) so the knobs stop changing them, but a locked line still
 * follows the chords and OCTAVE still transposes it.
 *
 * TIME. Positions are ticks at SEQ_PPQN (24 ticks = one 16th). Internal tempo
 * runs when nothing is clocking J1; the first clock edge takes over (and
 * starts the transport), and when edges stop for SEQ_CLOCK_LOST_MS the
 * transport stops and control goes back to the internal tempo. Between edges
 * the position is extrapolated from the measured period but never runs past
 * where the next edge will land. A reset edge makes the NEXT clock edge step
 * one (Eurorack convention); with no clock it rewinds at once.
 *
 * Output pitch is in volts, 1 V/oct: 0 V = C, plus the key root, plus the
 * voice's base octave (bass 1, lead 2), folded into 0..5 V by octaves. The
 * board adds calibration and clamps to the jack.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEQ_PPQN 96              /* ticks per quarter note */
#define SEQ_TPS (SEQ_PPQN / 4)   /* ticks per 16th step */
#define SEQ_BAR (SEQ_TPS * 16)   /* ticks per 4/4 bar */
#define SEQ_MAX_STEPS 64
#define SEQ_VOICES 2             /* generated voices */
#define SEQ_DRUMS 4              /* drum triggers */
#define SEQ_VOICE_SLOTS 8        /* patterns per generated voice */
#define SEQ_DRUM_SLOTS 16        /* drum patterns */
#define SEQ_MAX_CHORDS 8
#define SEQ_CLOCK_LOST_MS 2000   /* no clock edge this long -> external lost */
#define SEQ_TRIG_MS 5            /* drum trigger pulse */

enum { SEQ_BASS = 0, SEQ_LEAD = 1 };
enum { SEQ_KICK = 0, SEQ_SNARE = 1, SEQ_HAT = 2, SEQ_PERC = 3 };

typedef enum {
    SEQ_SCALE_MAJOR = 0,
    SEQ_SCALE_MINOR,
    SEQ_SCALE_DORIAN,
    SEQ_SCALE_PHRYGIAN,
    SEQ_SCALE_LYDIAN,
    SEQ_SCALE_MIXOLYDIAN,
    SEQ_SCALE_HARMONIC_MINOR,
    SEQ_SCALE_MINOR_PENT,
    SEQ_SCALE_MAJOR_PENT,
    SEQ_SCALE_COUNT
} seq_scale_t;

/* Knobs of a generated voice. All 0..1 unless noted. */
typedef struct {
    float   density;   /* chance a step plays */
    float   length;    /* 0 = staccato .. 1 = long, tied notes */
    float   slide;     /* chance a note glides from the one before */
    float   jump;      /* chance of an octave jump on a step */
    float   tone_bias; /* 0 = chord tones only .. 1 = always scale tones */
    float   swing;     /* how much of the global swing this voice takes */
    int8_t  octave;    /* -3..+3, transposes everything incl. a locked line */
    uint8_t range;     /* 1..3 octaves of pitch spread */
    uint8_t steps;     /* loop length 1..SEQ_MAX_STEPS */
    uint8_t div;       /* 16ths per step: 1 = 16th, 2 = 8th, 4 = quarter */
} seq_knobs_t;

/* One step as it will play. */
typedef struct {
    bool    on;
    bool    slide;    /* glide into this note, gate held from the last one */
    bool    scale;    /* index counts scale tones, not chord tones */
    int8_t  jump;     /* octave jump, -1, 0, +1 */
    uint8_t index;    /* tone index into the chord (or scale) */
    uint8_t len;      /* gate length in 1/16ths of a step, 2..32; 16 = the
                         whole step (still retriggered), above = tied */
} seq_step_t;

/* A generated voice's pattern slot. */
typedef struct {
    uint32_t    seed[SEQ_MAX_STEPS];
    uint32_t    undo[SEQ_MAX_STEPS];
    bool        has_undo;
    seq_knobs_t knobs;
    bool        locked;
    seq_step_t  lock[SEQ_MAX_STEPS];
} seq_pattern_t;

/* A drum pattern slot: per voice, per step, 0 = off, 1..4 = 25..100 %. */
typedef struct {
    uint8_t prob[SEQ_DRUMS][SEQ_MAX_STEPS];
    uint8_t steps[SEQ_DRUMS]; /* per-voice loop length (polymeter) */
    /* The generator, added on top of the programmed steps: a generated hit
     * fires at 100 % (it wins over a programmed 25-75 % step). */
    float   gen[SEQ_DRUMS];   /* amount 0..1 per drum; 0 = programmed only */
    uint8_t style;            /* seq_drum_style_t */
} seq_drums_t;

/* How a drum pattern's generator places hits.
 *   GEN 1-4    the voices' rule: every step owns a fixed number and the
 *              amount is a threshold against it, so raising it only adds
 *              hits and turning it back returns the same beat. Kicks come
 *              in on the beat first, snares on 2 and 4, hats on the 8ths.
 *              The four are four fixed sets of numbers.
 *   EUCLID 1-4 the amount is a hit count, round(amount x loop length),
 *              spread as evenly as the loop allows (Bjorklund / Toussaint),
 *              each drum rotated by a per-style table so E(2,16) on the
 *              snare lands on 2 and 4. */
typedef enum {
    SEQ_DSTYLE_GEN1 = 0, SEQ_DSTYLE_GEN2, SEQ_DSTYLE_GEN3, SEQ_DSTYLE_GEN4,
    SEQ_DSTYLE_EUCLID1, SEQ_DSTYLE_EUCLID2, SEQ_DSTYLE_EUCLID3, SEQ_DSTYLE_EUCLID4,
    SEQ_DSTYLE_COUNT
} seq_drum_style_t;

typedef struct {
    int8_t  degree; /* scale degree of the chord root, 0-based */
    uint8_t bars;   /* 1..8 */
} seq_chord_t;

typedef struct {
    uint8_t     root;  /* key, 0 = C .. 11 = B */
    uint8_t     scale; /* seq_scale_t */
    uint8_t     tones; /* chord size, 3 = triads, 4 = sevenths */
    uint8_t     count; /* chords in the progression, 1..SEQ_MAX_CHORDS */
    seq_chord_t chord[SEQ_MAX_CHORDS];
} seq_prog_t;

typedef struct {
    float pitch[SEQ_VOICES]; /* volts, 1 V/oct */
    bool  gate[SEQ_VOICES];
    bool  trig[SEQ_DRUMS];
} seq_out_t;

/* Per generated voice, runtime only. */
typedef struct {
    uint8_t  slot, queued;   /* queued == slot when nothing is queued */
    bool     mute;
    bool     gate;
    bool     tied;           /* the sounding note is longer than its step */
    double   gate_off;       /* tick at which the gate falls */
    float    pitch, target;  /* semitones above 0 V */
    float    glide_rate;     /* semitones per ms, 0 = arrived */
    bool     dip;            /* gate forced low briefly between two notes */
    uint32_t dip_until;      /* us */
    int32_t  cur_step;       /* last step started, -1 = none (for the UI) */
} seq_vstate_t;

typedef struct {
    seq_pattern_t pat[SEQ_VOICES][SEQ_VOICE_SLOTS];
    seq_drums_t   drums[SEQ_DRUM_SLOTS];
    seq_prog_t    prog;
    uint8_t       base_octave[SEQ_VOICES]; /* octaves above 0 V (= C):
                                              bass 1, lead 2 */

    /* global */
    float    bpm;       /* internal tempo, 30..300 */
    float    swing;     /* 0.5 = straight .. 0.75 = hard shuffle */
    uint16_t glide_ms;  /* slide time */
    uint8_t  clock_ppqn;/* pulses per quarter on J1: 1, 2, 4 (16ths), 24 */

    /* runtime */
    seq_vstate_t v[SEQ_VOICES];
    uint8_t  drum_slot, drum_queued;
    bool     drum_mute[SEQ_DRUMS];
    uint32_t trig_until[SEQ_DRUMS]; /* us timestamps */
    bool     trig_on[SEQ_DRUMS];
    int32_t  drum_step[SEQ_DRUMS];

    bool     running;
    bool     hold;       /* stopped by the player: J1 edges keep time only */
    double   pos;        /* ticks since the transport started */
    uint32_t last_us;
    bool     have_last;

    bool     ext;        /* following the external clock */
    uint32_t edges;      /* edges since the external clock took over */
    uint32_t edge_us;    /* time of the latest edge */
    uint32_t period_us;  /* measured edge period, 0 = unknown */
    bool     reset_pending;

    uint32_t rng;        /* runtime dice: drum probability, mutate, roll */

    uint32_t drums_file_crc; /* CRC of the last drum file imported (kept) */
} seq_t;

/* Setup. Everything starts stopped on internal clock at 120 BPM. */
void seq_init(seq_t *s, uint32_t seed);

/* Transport. Play restarts from the top -- at once on the internal clock,
 * on the next J1 edge when following one. Stop holds the transport until
 * the next play, even while J1 keeps clocking (the clock is still
 * measured). A clock that goes missing stops it too, but not as a hold:
 * the next clock edge starts it again. */
void seq_play(seq_t *s);
void seq_stop(seq_t *s);

/* Edges from J1 / J2. */
void seq_clock_edge(seq_t *s, uint32_t now_us);
void seq_reset_edge(seq_t *s, uint32_t now_us);

/* Advance to now_us and write the outputs. */
void seq_process(seq_t *s, uint32_t now_us, seq_out_t *out);

/* Generated voices. These act on the voice's current slot. The knobs pointer
 * changes when a queued pattern takes over at a bar line: fetch it each
 * time, don't keep it. */
seq_knobs_t *seq_knobs(seq_t *s, int voice);
void seq_roll(seq_t *s, int voice);
void seq_mutate(seq_t *s, int voice, float amount); /* 0..1 of the steps */
void seq_undo(seq_t *s, int voice);                 /* again = redo */
void seq_lock(seq_t *s, int voice, bool lock);
seq_step_t seq_step(const seq_t *s, int voice, int step); /* as it plays */
/* The note (semitones above the voice's 0 V) a step would play right now. */
float seq_step_note(const seq_t *s, int voice, int step);

/* Patterns switch on the next bar line. */
void seq_voice_queue(seq_t *s, int voice, int slot);
void seq_voice_copy(seq_t *s, int voice, int from, int to);
void seq_drum_queue(seq_t *s, int slot);
void seq_voice_mute(seq_t *s, int voice, bool mute);
void seq_drum_mute(seq_t *s, int drum, bool mute);
/* Edit the current drum pattern. prob 0 = off, 1..4 = 25..100 %. */
void seq_drum_set(seq_t *s, int drum, int step, int prob);

/* The current drum pattern (the one playing), for the UI to edit its
 * generator: gen[] and style. Fetch it each time; it changes at bar lines. */
seq_drums_t *seq_drums(seq_t *s);
/* What a drum step does now in the current pattern: 0 off, 1..4 = 25..100 %
 * (a generated hit counts as 4). *generated, if given, says the hit comes
 * from the generator rather than a programmed step. */
int seq_drum_prob(const seq_t *s, int drum, int step, bool *generated);
/* One Euclidean rhythm: is step k (0..n-1) a hit of E(hits, n) rotated by
 * rot steps? Exposed for tests and the manual. */
bool seq_euclid(int hits, int n, int rot, int k);

/* Drum patterns as text (the SD card's /seq/drums.txt):
 *
 *   pattern 3            which slot the lines below go to, 1..16
 *   kick  x...x...x...x...
 *   snare ....x.......x...
 *   hat   ..x.3.x.2.x.1.x.
 *   perc  x..x..x..        (a line's length is that drum's loop: 1..64)
 *
 * x = 100 %, 3 / 2 / 1 = 75 / 50 / 25 %, . or - = off; spaces inside a
 * line are ignored. '#' starts a comment. Drums may be missing (they are
 * left as they are); lines before the first "pattern" go to pattern 1.
 * Only the patterns the text names change, and a named pattern's generator
 * is switched off so the file is what plays. Returns how many patterns
 * were written, or -1 (and changes nothing) if a line makes no sense. */
int seq_drums_parse(seq_t *s, const char *text, size_t len);

/* Where things are, for the Launchpad and the rings. */
int seq_chord_index(const seq_t *s); /* chord playing now */

/* Persistence. What a power cycle should keep -- the seeds, knobs, locks,
 * drum patterns, progression, tempo and which slots are playing -- as a flat
 * byte stream of SEQ_STATE_BYTES, field by field (never the struct, whose
 * layout mixes in runtime state; undo is not kept). Reading clamps every
 * field into range, so a corrupt blob makes an odd pattern, never a crash;
 * a wrong version byte returns false and changes nothing. Reading stops the
 * transport and clears the runtime state. */
#define SEQ_STATE_VERSION 2
#define SEQ_STATE_BYTES 15600
void seq_state_write(const seq_t *s, uint8_t *out);
bool seq_state_read(seq_t *s, const uint8_t *in);
size_t seq_state_used(void); /* bytes of SEQ_STATE_BYTES in use */

/* Scale and chord helpers (exposed for tests and the UI). */
int seq_scale_len(int scale);
int seq_degree_semis(int scale, int degree); /* any degree, octaves carry */

#ifdef __cplusplus
}
#endif
