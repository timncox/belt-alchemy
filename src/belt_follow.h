/*
 * FOLLOW: the harmony voices pick a chord from what you sing.
 *
 * Belt's normal harmonies copy the sung note at fixed intervals, so they
 * move on every note. FOLLOW holds a chord instead and changes it only when
 * a new note has been sung for STICK: the voices sing a chord in the KEY and
 * SCALE that fits the note, voiced the way the mode asks.
 *
 *   ROOT      the sung note is the chord's root: 3rd and 5th above it, the
 *             root an octave below, and a 4th voice that is the 5th below
 *             (triad), the 7th above (7th) or the 9th above (9th).
 *   S A T B   the sung note is that part of a four-part chord. A chord in
 *             the scale that holds the note is chosen (staying put when it
 *             can, I / IV / V before the others, roots moving by 4ths and
 *             5ths), and the other three parts move by the smallest steps
 *             with the usual spacing and no parallel 5ths or octaves.
 *   CREATIVE  the sung note sits on top as a colour tone -- a 9th, 7th,
 *             6th or 11th of the chord under it -- in an open voicing with
 *             the root in the bass. COLOR sets how far out it reaches.
 *
 * Pure functions on plain ints: no engine state, no allocation, so the
 * native test can call them directly and the engine calls them at most a
 * few times a second (only when a new chord is committed).
 */
#ifndef BELT_FOLLOW_H
#define BELT_FOLLOW_H

#ifdef __cplusplus
extern "C" {
#endif

/* follow param values (module.json order) */
#define FOLLOW_OFF      0
#define FOLLOW_ROOT     1
#define FOLLOW_SOPRANO  2
#define FOLLOW_ALTO     3
#define FOLLOW_TENOR    4
#define FOLLOW_BASS     5
#define FOLLOW_CREATIVE 6
#define FOLLOW_MODES    7

/* follow_size values */
#define FOLLOW_TRIAD 0
#define FOLLOW_7TH   1
#define FOLLOW_9TH   2

#define FOLLOW_VOICES 4      /* = BELT_HARMONIES */
#define FOLLOW_PARTS  5      /* the sung note + up to four voices */

typedef struct {
    int root_pc;             /* 0-11, -1 = no chord yet */
    int third, fifth, seventh, ninth;   /* semitones above the root */
    int degree;              /* scale degree of the root, -1 = outside the scale */
} follow_chord_t;

typedef struct {
    /* the committed chord and what each harmony voice sings (MIDI, -1 off) */
    follow_chord_t chord;
    int notes[FOLLOW_VOICES];
    int melody;              /* sung note the chord was built on, -1 none */
    /* the whole voicing, low to high, for voice leading into the next one */
    int parts[FOLLOW_PARTS];
    int n_parts;
    int mel_idx;
    int mode;                /* mode the voicing was built in */
} follow_state_t;

void follow_reset(follow_state_t *f);

/* Build the chord for a newly committed sung note `melody` (MIDI) and
 * update f->notes. key 0-11, scale = Belt's scale index (0 Chromatic,
 * 1 Major ... 8 Blues). bass: 0 = no voice below the sung note's chord
 * root (ROOT, CREATIVE) / no bass part (S A T). color 0-100. */
void follow_commit(follow_state_t *f, int melody, int key, int scale,
                   int mode, int size, int bass, int color);

#ifdef __cplusplus
}
#endif

#endif /* BELT_FOLLOW_H */
