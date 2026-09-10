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
    assert(versio_alloc_capacity() - versio_alloc_used() >= 256u * 1024u);

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
