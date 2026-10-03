/*
 * The chord sources' rules (src/chord_src.c) against seq-alchemy's own core
 * (src/vendor/seq.c), no hardware: which notes a progression holds, when
 * the internal tempo changes chord, the note-off / note-on diff, the CV
 * inputs' rounding and hysteresis, and the calibration step.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chord_src.h"

static int fails = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } \
    } while (0)

/* emitted messages */
static uint8_t ev[64][3];
static int     nev = 0;
static void emit(uint8_t st, uint8_t note, uint8_t vel, void *ctx)
{
    (void)ctx;
    if (nev < 64) { ev[nev][0] = st; ev[nev][1] = note; ev[nev][2] = vel; nev++; }
}

static seq_t S;

static void test_chords(void)
{
    seq_init(&S, 1);
    uint8_t n[4];
    /* seq's default: A minor, i - VI - III - VII. C3 = 48. */
    int k = cs_chord_notes(&S.prog, 0, 48, n);
    CHECK(k == 3 && n[0] == 57 && n[1] == 60 && n[2] == 64, "i = A3 C4 E4 (%d: %d %d %d)", k, n[0], n[1], n[2]);
    k = cs_chord_notes(&S.prog, 1, 48, n);
    CHECK(k == 3 && n[0] == 65 && n[1] == 69 && n[2] == 72, "VI = F4 A4 C5 (%d %d %d)", n[0], n[1], n[2]);
    k = cs_chord_notes(&S.prog, 3, 48, n);
    CHECK(k == 3 && n[0] == 64 + 3 && n[1] == 71 && n[2] == 74, "VII = G4 B4 D5 (%d %d %d)", n[0], n[1], n[2]);
    S.prog.tones = 4;
    k = cs_chord_notes(&S.prog, 0, 48, n);
    CHECK(k == 4 && n[3] == 67, "i7 adds G4 (%d)", n[3]);
    CHECK(cs_chord_notes(&S.prog, 4, 48, n) == 0, "past the progression: nothing");
    /* key + scale from Belt: C major, I */
    S.prog.root = 0; S.prog.scale = (uint8_t)cs_seq_scale(1); S.prog.tones = 3;
    S.prog.chord[0].degree = 0;
    k = cs_chord_notes(&S.prog, 0, 60, n);
    CHECK(n[0] == 60 && n[1] == 64 && n[2] == 67, "C major I at C4 = C E G");
    CHECK(cs_seq_scale(0) == SEQ_SCALE_MAJOR && cs_seq_scale(8) == SEQ_SCALE_MINOR_PENT
          && cs_seq_scale(3) == SEQ_SCALE_HARMONIC_MINOR, "scale map");
}

static void test_hold(void)
{
    cs_held_t h = {{0}, 0};
    const uint8_t a[3] = {57, 60, 64}, b[3] = {65, 69, 72}, c[4] = {60, 64, 64, 69};
    nev = 0;
    cs_hold(&h, a, 3, 100, emit, NULL);
    CHECK(nev == 3 && ev[0][0] == 0x90 && ev[0][1] == 57 && ev[2][1] == 64, "first chord: 3 ons");
    nev = 0;
    cs_hold(&h, b, 3, 100, emit, NULL);
    CHECK(nev == 6, "a new chord: 3 offs + 3 ons (%d)", nev);
    CHECK(ev[0][0] == 0x80 && ev[1][0] == 0x80 && ev[2][0] == 0x80 && ev[3][0] == 0x90, "offs before ons");
    nev = 0;
    cs_hold(&h, a, 3, 100, emit, NULL);
    nev = 0;
    cs_hold(&h, c, 4, 100, emit, NULL);   /* A C E -> C E E A: only A3 -> A4 moves */
    CHECK(nev == 2 && ev[0][0] == 0x80 && ev[0][1] == 57 && ev[1][0] == 0x90 && ev[1][1] == 69,
          "common tones stay, duplicates dropped (%d msgs)", nev);
    CHECK(h.n == 3, "held 3 distinct (%d)", h.n);
    nev = 0;
    cs_release(&h, emit, NULL);
    CHECK(nev == 3 && h.n == 0 && ev[2][0] == 0x80, "release: every held note off");
}

/* The internal tempo moves the chord on the bar lines. */
static void test_tempo(void)
{
    seq_init(&S, 1);
    S.bpm = 120.0f;   /* a 4/4 bar = 2 s; four 1-bar chords */
    seq_play(&S);
    int      last = -1, changes = 0;
    uint32_t at[8];
    for (uint32_t ms = 0; ms <= 8500; ms++)
    {
        seq_process(&S, ms * 1000u, NULL);
        const int ci = seq_chord_index(&S);
        if (ci != last)
        {
            if (changes < 8) at[changes] = ms;
            changes++;
            last = ci;
        }
    }
    CHECK(changes == 5, "chords 0 1 2 3 0 in 8.5 s (%d)", changes);
    CHECK(at[0] == 0 && abs((int)at[1] - 2000) <= 1 && abs((int)at[2] - 4000) <= 1
          && abs((int)at[3] - 6000) <= 1 && abs((int)at[4] - 8000) <= 1,
          "on the bar lines (%u %u %u %u %u)", at[0], at[1], at[2], at[3], at[4]);
    S.prog.chord[1].bars = 2;   /* VI for two bars */
    seq_play(&S);
    last = 0;
    uint32_t t = 0;
    for (uint32_t ms = 0; ms <= 7000; ms++)
    {
        seq_process(&S, (8501u + ms) * 1000u, NULL);   /* time runs on: a gap would be a jump */
        if (seq_chord_index(&S) == 2 && last != 2) t = ms;
        last = seq_chord_index(&S);
    }
    CHECK(abs((int)t - 6000) <= 2, "a two-bar chord holds two bars (III at %u ms)", t);
}

static float noise(void) { return ((float)rand() / (float)RAND_MAX - 0.5f) * 0.010f; } /* +-5 mV */

static void test_cv(void)
{
    cs_cv_t c;
    cs_cv_init(&c);
    float v[4] = {0.25f, 4.0f / 12.0f, 7.0f / 12.0f, 1.0f};
    CHECK(cs_cv_poll(&c, v, 4), "first poll commits");
    uint8_t n[4];
    cs_cv_notes(&c, 4, 48, n);
    CHECK(n[0] == 51 && n[1] == 52 && n[2] == 55 && n[3] == 60, "0.25 V = 3 st (%d %d %d %d)", n[0], n[1], n[2], n[3]);

    /* sitting still with +-5 mV noise: nothing moves */
    int moved = 0;
    for (int i = 0; i < 2000; i++)
    {
        float w[4];
        for (int j = 0; j < 4; j++) w[j] = v[j] + noise();
        moved += cs_cv_poll(&c, w, 4);
    }
    CHECK(moved == 0, "noise does not move a note (%d)", moved);

    /* parked on a boundary (half a semitone) with noise: no chatter */
    cs_cv_init(&c);
    float b[1] = {0.5f / 12.0f};
    cs_cv_poll(&c, b, 1);
    moved = 0;
    for (int i = 0; i < 5000; i++)
    {
        float w[1] = {b[0] + noise() * 2.0f};
        moved += cs_cv_poll(&c, w, 1);
    }
    CHECK(moved == 0, "a boundary with +-10 mV noise holds (%d changes)", moved);

    /* a step of one semitone lands, quickly */
    cs_cv_init(&c);
    float z[1] = {0.0f};
    cs_cv_poll(&c, z, 1);
    float s[1] = {1.0f / 12.0f};
    int   ms = 0;
    while (ms < 100 && !cs_cv_poll(&c, s, 1)) ms++;
    cs_cv_notes(&c, 1, 48, n);
    CHECK(n[0] == 49 && ms <= 15, "a semitone up lands in %d ms as %d", ms + 1, n[0]);
    /* a jump of a fifth lands once, not on every note on the way */
    float f[1] = {8.0f / 12.0f};
    moved = 0;
    for (int i = 0; i < 50; i++) moved += cs_cv_poll(&c, f, 1);
    cs_cv_notes(&c, 1, 48, n);
    CHECK(n[0] == 56 && moved == 1, "a jump commits once (%d times) to %d", moved, n[0]);

    /* a sender off by up to the rounding margin still lands on its note,
     * from either side: C3 -> D3 +40 mV, then -> E3 -40 mV */
    cs_cv_init(&c);
    cs_cv_poll(&c, z, 1);
    float d[1] = {2.0f / 12.0f + 0.040f};
    for (int i = 0; i < 50; i++) cs_cv_poll(&c, d, 1);
    cs_cv_notes(&c, 1, 48, n);
    CHECK(n[0] == 50, "D3 +40 mV lands on D3 (%d)", n[0]);
    float e[1] = {4.0f / 12.0f - 0.040f};
    for (int i = 0; i < 50; i++) cs_cv_poll(&c, e, 1);
    cs_cv_notes(&c, 1, 48, n);
    CHECK(n[0] == 52, "E3 -40 mV lands on E3 (%d)", n[0]);
}

static void test_cal(void)
{
    int16_t off[4] = {0, 0, 0, 77};
    const float meas[4] = {0.030f, 4.0f / 12.0f - 0.045f, 7.0f / 12.0f + 0.010f, 1.0f + 0.4f};
    const unsigned ok = cs_cal_learn(meas, 4, off);
    CHECK(ok == 0x7u, "three learned, the miswired one refused (mask %x)", ok);
    CHECK(off[0] == 30 && off[1] == -45 && off[2] == 10 && off[3] == 77, "offsets %d %d %d, kept %d",
          off[0], off[1], off[2], off[3]);

    /* The same sender error on a played chord: uncorrected, the -45 mV
     * input is a semitone flat; corrected, every note lands. (+-41.7 mV is
     * the whole margin of 1 V/oct rounding.) */
    const float chord[3] = {2.0f / 12.0f + 0.030f, 6.0f / 12.0f - 0.045f, 9.0f / 12.0f + 0.010f};
    cs_cv_t c;
    uint8_t n[3];
    cs_cv_init(&c);
    cs_cv_poll(&c, chord, 3);
    cs_cv_notes(&c, 3, 48, n);
    CHECK(n[0] == 50 && n[1] == 53 && n[2] == 57, "uncorrected: F#3 reads F3 (%d %d %d)", n[0], n[1], n[2]);
    float fixed[3];
    for (int i = 0; i < 3; i++) fixed[i] = chord[i] - off[i] / 1000.0f;
    cs_cv_init(&c);
    cs_cv_poll(&c, fixed, 3);
    cs_cv_notes(&c, 3, 48, n);
    CHECK(n[0] == 50 && n[1] == 54 && n[2] == 57, "corrected: D3 F#3 A3 (%d %d %d)", n[0], n[1], n[2]);
}

int main(void)
{
    srand(7);
    test_chords();
    test_hold();
    test_tempo();
    test_cv();
    test_cal();
    printf("test_chord_src: %s (%d failures)\n", fails ? "FAIL" : "pass", fails);
    return fails ? 1 : 0;
}
