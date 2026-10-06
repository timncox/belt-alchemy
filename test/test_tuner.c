/*
 * Belt on the SDRAM bump allocator, at 48 kHz, natively.
 *
 * 1. belt_create() lands in the firmware's pool (VERSIO_POOL_BYTES) with
 *    nothing failed and room to spare.
 * 2. A 440 Hz sine is read as A4 (MIDI 69) and a C3 sine as MIDI 48, to a
 *    tenth of a semitone. This is what the vendored header's BELT_SR edit is
 *    for: at 44100 the same input reads 67.5 and 46.5.
 * 3. Hard-tune in C major pulls an A that is 35 cents sharp back to 440 Hz,
 *    measured by zero crossings on the output -- correction runs, and runs
 *    in the right direction, at this sample rate.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/versio_alloc.h"
#include "belt_core.h"

#define BLK 128
#define SR  BELT_SR

static float fake_bpm(void) { return 120.0f; }

static double get_num(belt_t *b, const char *k)
{
    char buf[64];
    assert(belt_get_param(b, k, buf, sizeof buf) > 0);
    return atof(buf);
}

/* Feed `seconds` of a stereo sine at `hz`, amplitude 0.3, collecting the
 * left output into `out` (may be NULL). Returns frames written. */
static long run_sine(belt_t *b, double hz, double seconds, double *phase,
                     int16_t *out, long out_cap)
{
    int16_t io[BLK * 2];
    long    total = (long)(seconds * SR);
    long    done  = 0;
    while (done < total)
    {
        for (int i = 0; i < BLK; i++)
        {
            int16_t v = (int16_t)(0.3 * 32767.0 * sin(*phase));
            *phase += 2.0 * M_PI * hz / SR;
            io[2 * i] = io[2 * i + 1] = v;
        }
        belt_process(b, io, io, BLK);
        if (out)
            for (int i = 0; i < BLK && done + i < out_cap; i++) out[done + i] = io[2 * i];
        done += BLK;
    }
    return done;
}

/* A sung-like note: harmonics up to 5 kHz, 1/k with two formant bumps. */
static long run_voice(belt_t *b, double hz, double seconds, double *phase,
                      int16_t *out, long out_cap)
{
    int16_t io[BLK * 2];
    long    total = (long)(seconds * SR);
    long    done  = 0;
    int     nh    = (int)(5000.0 / hz);
    while (done < total)
    {
        for (int i = 0; i < BLK; i++)
        {
            double s = 0.0;
            for (int k = 1; k <= nh; k++)
            {
                double fk  = k * hz;
                double env = (1.0 + 2.0 * exp(-pow((fk - 700.0) / 250.0, 2)) +
                              1.2 * exp(-pow((fk - 1200.0) / 300.0, 2))) / k;
                s += env * sin(k * *phase);
            }
            *phase += 2.0 * M_PI * hz / SR;
            int16_t v = (int16_t)(0.12 * 32767.0 * s);
            io[2 * i] = io[2 * i + 1] = v;
        }
        belt_process(b, io, io, BLK);
        if (out)
            for (int i = 0; i < BLK && done + i < out_cap; i++) out[done + i] = io[2 * i];
        done += BLK;
    }
    return done;
}

/* Hann-windowed power of x at `hz` (Goertzel). */
static double goertzel(const int16_t *x, long n, double hz)
{
    double c = 2.0 * cos(2.0 * M_PI * hz / SR), s1 = 0.0, s2 = 0.0;
    for (long i = 0; i < n; i++)
    {
        double w  = 0.5 - 0.5 * cos(2.0 * M_PI * i / n);
        double s0 = x[i] / 32768.0 * w + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* Fundamental by zero crossings over the buffer, Hz. */
static double zc_freq(const int16_t *x, long n)
{
    long crossings = 0;
    for (long i = 1; i < n; i++)
        if ((x[i - 1] < 0) != (x[i] < 0)) crossings++;
    return (double)crossings * SR / (2.0 * (double)n);
}

int main(void)
{
    void *pool = malloc(VERSIO_POOL_BYTES);
    assert(pool);
    versio_alloc_init(pool, VERSIO_POOL_BYTES);

    host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version      = 1;
    host.sample_rate      = SR;
    host.frames_per_block = BLK;
    host.get_bpm          = fake_bpm;

    belt_t *b = belt_create(&host);
    assert(b);
    assert(!versio_alloc_failed());
    printf("pool %zu B, engine took %zu B\n", versio_alloc_capacity(), versio_alloc_used());
    /* Headroom in the engine pool. 7bdba15 moved the pool into D2 SRAM and
     * sized it at 160 KB for an engine of ~137 KB, but this line still asked
     * for 256 KB free and has failed since. What matters is that the engine
     * fits with a margin for its next small growth (PR #8's held-note state
     * added 160 B): 16 KB. */
    assert(versio_alloc_capacity() - versio_alloc_used() >= 16u * 1024u);

    /* 1. detection at 48 kHz: A4 and C3 */
    belt_set_param(b, "monitor", "1");
    belt_set_param(b, "amount",  "0");     /* just listen */
    double ph = 0.0;
    run_sine(b, 440.0, 1.0, &ph, NULL, 0);
    double note = get_num(b, "detected_note");
    double freq = get_num(b, "detected_freq");
    int    voiced = (int)get_num(b, "voiced");
    printf("440 Hz -> voiced %d, %.2f Hz, note %.2f\n", voiced, freq, note);
    assert(voiced == 1);
    assert(fabs(note - 69.0) < 0.1);
    assert(fabs(freq - 440.0) < 3.0);

    run_sine(b, 130.8128, 1.0, &ph, NULL, 0);
    note = get_num(b, "detected_note");
    freq = get_num(b, "detected_freq");
    printf("C3     -> %.2f Hz, note %.2f\n", freq, note);
    assert(fabs(note - 48.0) < 0.1);

    /* F2, near the bottom of the tracked range (BELT_FMIN 85 Hz). YIN's
     * longest lag was fixed at 260 for 22050 Hz analysis; at 48 kHz that is
     * 92 Hz, so this note was out of reach and low notes glitched. */
    run_sine(b, 87.3071, 1.0, &ph, NULL, 0);
    note = get_num(b, "detected_note");
    freq = get_num(b, "detected_freq");
    printf("F2     -> %.2f Hz, note %.2f\n", freq, note);
    assert(fabs(note - 41.0) < 0.1);

    /* F2 comes out clean, not just detected. Grains snapped to a mark up to
     * half a period past where the latency budget assumed, so at ~84-89 Hz
     * they read ~200 samples past the write head into 16K-old ring data:
     * digital noise on low notes (Tim, 2026-10-06). A sine can't show it --
     * the stale data is the same sine and adds back in tune -- so sing a
     * harmonic-rich F2 (nothing above 5 kHz) and measure what lands above
     * 6 kHz, relative to the harmonics. */
    belt_set_param(b, "harm1", "0");
    belt_set_param(b, "harm2", "0");
    belt_set_param(b, "harm3", "0");
    belt_set_param(b, "harm4", "0");
    belt_set_param(b, "double_amt", "0");
    belt_set_param(b, "wet", "100");
    {
        /* where a grain's mark lands against the budget depends on the mark
         * train's phase, i.e. on history -- so walk the bottom of the range
         * and judge the worst note */
        static const double lows[] = { 84.0, 85.5, 86.5, 87.3071, 88.0, 89.0 };
        double   worst = -200.0, vph = 0.0;
        long     fn    = SR;
        int16_t *fo    = calloc((size_t)fn, sizeof(int16_t));
        assert(fo);
        for (unsigned j = 0; j < sizeof lows / sizeof lows[0]; j++)
        {
            const double f2 = lows[j];
            run_voice(b, f2, 1.0, &vph, NULL, 0);           /* settle */
            run_voice(b, f2, 1.0, &vph, fo, fn);
            double hf = 0.0, harm = 0.0;
            for (double f = 6000.0; f <= 16000.0; f += 50.0) hf += goertzel(fo, fn, f);
            for (int k = 1; k * f2 < 5000.0; k++) harm += goertzel(fo, fn, k * f2);
            double hf_db = 10.0 * log10(hf / harm + 1e-12);
            if (hf_db > worst) worst = hf_db;
        }
        printf("84-89 Hz out -> above 6 kHz, worst %.1f dB re harmonics\n", worst);
        assert(worst < -70.0);   /* -58 before the fix, -80 after */
        free(fo);
    }

    /* 2. hard-tune pulls a sharp A back to A. C major, so A is a scale note. */
    belt_set_param(b, "key",    "0");
    belt_set_param(b, "scale",  "1");
    belt_set_param(b, "hard",   "1");
    belt_set_param(b, "wet",    "100");
    belt_set_param(b, "harm1",  "0");
    belt_set_param(b, "harm2",  "0");
    belt_set_param(b, "harm3",  "0");
    belt_set_param(b, "harm4",  "0");
    belt_set_param(b, "double_amt", "0");
    const double sharp = 440.0 * pow(2.0, 35.0 / 1200.0);   /* +35 cents */
    run_sine(b, sharp, 1.5, &ph, NULL, 0);                  /* settle */
    long    n   = SR;                                        /* one second */
    int16_t *o  = calloc((size_t)n, sizeof(int16_t));
    assert(o);
    run_sine(b, sharp, 1.0, &ph, o, n);
    const double fin  = sharp;
    const double fout = zc_freq(o, n);
    printf("hard-tune: in %.1f Hz -> out %.1f Hz\n", fin, fout);
    assert(fabs(fout - 440.0) < 4.0);
    assert(fabs(fout - 440.0) < fabs(fin - 440.0));
    free(o);

    puts("test_tuner: ok");
    free(pool);
    return 0;
}
