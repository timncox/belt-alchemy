/*
 * chord_src -- what the chord sources share, with no hardware in it, so the
 * native tests can run every rule here (test/test_chord_src.c).
 *
 * Every chord source ends as note-on / note-off messages into belt_on_midi()
 * (docs/alchemy-chord-vocoder-design.md: "the engine never knows where a
 * chord came from"). This file decides WHICH notes:
 *
 *   cs_hold          a source's chord moves: note-offs for the notes it no
 *                    longer holds, then note-ons for the new ones, never a
 *                    note twice
 *   cs_chord_notes   the internal sequencer's chord, from seq-alchemy's
 *                    progression (vendor/seq.[ch], copied unchanged)
 *   cs_cv_*          four 1 V/oct inputs to semitones: smoothed, rounded to
 *                    the nearest semitone with hysteresis, a new note only
 *                    once it has held still
 *   cs_cal_learn     the calibration step: a known reference chord on the
 *                    inputs, a per-input offset out
 *
 * vendor/seq.[ch] is seq-alchemy's core/seq.[ch] at worktree-core b4453ea,
 * byte-identical; seq-alchemy stays the source of truth. The chord rule
 * below (tone i of a chord on degree d is scale degree d + 2i) is seq.c's
 * note_for() for chord tones, which is file-static there.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vendor/seq.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CS_MAX_NOTES 4      /* Belt has four harmony voices */
#define CS_CV_INPUTS 4      /* J4..J7 */

/* status 0x90 note-on / 0x80 note-off, as belt_on_midi() takes them */
typedef void (*cs_emit_fn)(uint8_t status, uint8_t note, uint8_t vel, void *ctx);

/* The notes one source is holding right now. */
typedef struct {
    uint8_t note[CS_MAX_NOTES];
    uint8_t n;
} cs_held_t;

/* Make h hold notes[0..n) (duplicates and n > CS_MAX_NOTES dropped): offs
 * first for what it no longer holds, then ons for what is new. Notes held
 * before and after are left alone, so a voice on a common tone does not
 * re-attack. */
void cs_hold(cs_held_t *h, const uint8_t *notes, int n, uint8_t vel,
             cs_emit_fn emit, void *ctx);
void cs_release(cs_held_t *h, cs_emit_fn emit, void *ctx);

/* Belt's SCALE (belt_core.c order) as a seq_scale_t. Seq has no Chromatic
 * or Blues: Chromatic chords are Major's, Blues' are Minor pentatonic's. */
int cs_seq_scale(int belt_scale);

/* The notes of chord ci of the progression, lowest first: the chord root is
 * base + the key + the degree's semitones, the other tones stack every
 * second scale degree above it (triads 3, sevenths 4). base = the MIDI note
 * of the octave's C (48 = C3). Returns how many (0 if ci is out of range). */
int cs_chord_notes(const seq_prog_t *pr, int ci, int base, uint8_t *out);

/* The same, for any scale degree (0 = the key's root; negative and past the
 * scale's length go down / up octaves): the diatonic chord on that degree. */
int cs_degree_notes(int key, int scale, int tones, int degree, int base, uint8_t *out);

/* ---- one-jack CV chord (Tim, 2026-10-03; shared with seq-alchemy) -------
 *
 * One jack, 1 V/oct, 0 V = the Octave's C: the voltage is the chord ROOT.
 * It is snapped to the nearest note of KEY / SCALE; that scale degree's
 * diatonic chord is what is held, so the quality falls out of the scale (C
 * major: C Dm Em F G Am B dim). The degree moves once another scale note is
 * nearer than the held one by 2 x CS_CV_HYST, the input itself agrees, and
 * it has stayed CS_CV_STILL_MS polls; a KEY / SCALE change re-snaps at once.
 */
typedef struct {
    float   smooth;
    int     degree, cand;    /* scale degrees from the key's root */
    uint8_t still;
    int     key, scale;      /* what degree was snapped against */
    bool    primed;
} cs_cvdeg_t;

void cs_cvdeg_init(cs_cvdeg_t *c);
/* One 1 ms poll; volts with the offset taken off. key 0..11, scale a
 * seq_scale_t. Returns true when the degree changed. */
bool cs_cvdeg_poll(cs_cvdeg_t *c, float volts, int key, int scale);

/* ---- CV pitch inputs -------------------------------------------------- */

/* A held note moves once the smoothed pitch is half a semitone plus
 * CS_CV_HYST (0.15 st = 12.5 mV) from it, to the nearest semitone, and has
 * stayed there CS_CV_STILL_MS polls. */
#define CS_CV_HYST     0.15f
#define CS_CV_STILL_MS 3

typedef struct {
    float   smooth[CS_CV_INPUTS];  /* volts, one-pole */
    int16_t semis[CS_CV_INPUTS];   /* committed: semitones above 0 V */
    int16_t cand[CS_CV_INPUTS];
    uint8_t still[CS_CV_INPUTS];
    bool    primed;
} cs_cv_t;

void cs_cv_init(cs_cv_t *c);
/* One 1 ms poll. volts[] already has the calibration offset taken off.
 * Returns true when a committed semitone changed. */
bool cs_cv_poll(cs_cv_t *c, const float *volts, int n);
/* The committed notes of inputs 0..n-1: base + semitones (0 V = base),
 * clamped to 0..127. Returns n. */
int cs_cv_notes(const cs_cv_t *c, int n, int base, uint8_t *out);

/* ---- calibration -------------------------------------------------------- */

/* The reference chord the sender plays: J4 0 V, J5 +4/12 V, J6 +7/12 V,
 * J7 +1 V -- C, E, G and the C above, whatever octave 0 V is. */
extern const float cs_cal_ref[CS_CV_INPUTS];
/* An offset bigger than this is a wrong patch, not an error to learn. */
#define CS_CAL_MAX_V 0.25f

/* Learn each input's offset (measured - reference) in millivolts. Inputs
 * whose offset is implausible keep their old one. Returns the bitmask of
 * inputs learned. */
unsigned cs_cal_learn(const float *measured, int n, int16_t *offset_mv);

#ifdef __cplusplus
}
#endif
