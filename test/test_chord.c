/*
 * Hide and Seek, natively, at 48 kHz, through the firmware's own
 * translation unit (belt_core_alchemy.c on the SDRAM bump allocator).
 *
 * A voice-like A3 (a band-limited buzz: TD-PSOLA keeps the input's
 * spectral envelope, so a pure sine shifted up a sixth has almost nothing
 * to sound with -- see the upstream sim test 24) is sung while C4-E4-G4
 * are held as MIDI notes, the way the Launchpad's PLAY pads send them.
 *
 * 1. LEAD 100: the lead (A3) is in the output.
 * 2. LEAD 0 (chord only): the three held notes sound, each at its pitch,
 *    at comparable levels, and the A3 lead and dry voice are gone.
 * 3. Released: silence.
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
#define CAP 16384

static float fake_bpm(void) { return 120.0f; }

static float cap[CAP];   /* last CAP output frames, mono */
static long  cap_n = 0;

static void run_voice(belt_t *b, double hz, double seconds, double *phase)
{
    int16_t io[BLK * 2];
    int     nh = (int)(20000.0 / hz);
    if (nh > 40) nh = 40;
    double norm = 0.0;
    for (int k = 1; k <= nh; k++) norm += 1.0 / k;
    long total = (long)(seconds * SR);
    for (long done = 0; done < total; done += BLK)
    {
        for (int i = 0; i < BLK; i++)
        {
            double x = 0.0;
            for (int k = 1; k <= nh; k++) x += sin(*phase * k) / k;
            *phase += 2.0 * M_PI * hz / SR;
            if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
            int16_t v = (int16_t)(x / norm * 2.2 * 0.35 * 32767.0);
            io[2 * i] = io[2 * i + 1] = v;
        }
        belt_process(b, io, io, BLK);
        for (int i = 0; i < BLK; i++)
            cap[(cap_n++) % CAP] = 0.5f * ((float)io[2 * i] + (float)io[2 * i + 1]) / 32768.0f;
    }
}

/* Goertzel power at hz over the captured output. */
static double power(double hz)
{
    double w = 2.0 * M_PI * hz / SR, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0;
    for (long i = 0; i < CAP; i++)
    {
        double s0 = cap[(cap_n + i) % CAP] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / CAP;
}

static double rms(void)
{
    double a = 0.0;
    for (long i = 0; i < CAP; i++) a += (double)cap[i] * cap[i];
    return sqrt(a / CAP);
}

static void note(belt_t *b, int on, int n)
{
    uint8_t m[3] = {(uint8_t)(on ? 0x90 : 0x80), (uint8_t)n, (uint8_t)(on ? 100 : 0)};
    belt_on_midi(b, m, 3, 0);
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
    assert(b && !versio_alloc_failed());

    /* The firmware's defaults: monitor on, a Eurorack input. Intervals Off
     * so only held notes sing; wet 50 so the dry voice is in the mix too. */
    belt_set_param(b, "monitor", "1");
    belt_set_param(b, "hw_input", "1");
    belt_set_param(b, "midi_mode", "1");
    for (int i = 1; i <= 4; i++)
    {
        char k[8];
        snprintf(k, sizeof k, "harm%d", i);
        belt_set_param(b, k, "0");
    }
    belt_set_param(b, "wet", "50");
    belt_set_param(b, "humanize", "0");

    static const int    nn[3]   = {60, 64, 67};
    static const double want[3] = {261.63, 329.63, 392.00};
    double ph = 0.0;
    for (int k = 0; k < 3; k++) note(b, 1, nn[k]);

    run_voice(b, 220.0, 2.0, &ph);
    const double lead_on = power(220.0);

    belt_set_param(b, "lead", "0");
    run_voice(b, 220.0, 2.0, &ph);
    const double lead_off = power(220.0);
    double p[3];
    for (int k = 0; k < 3; k++) p[k] = power(want[k]);
    const double off_note = power(311.13);   /* D#4, not held */

    printf("48 kHz chord only: A3 lead %.4f -> %.5f | C4 %.3f E4 %.3f G4 %.3f | D#4 %.5f\n",
           lead_on, lead_off, p[0], p[1], p[2], off_note);
    assert(lead_on > 20.0 * lead_off);   /* lead + dry really removed */
    double lo = p[0], hi = p[0];
    for (int k = 0; k < 3; k++)
    {
        assert(p[k] > 10.0 * off_note);  /* every held note, at its pitch */
        if (p[k] < lo) lo = p[k];
        if (p[k] > hi) hi = p[k];
    }
    assert(hi < 4.0 * lo);               /* comparable levels */

    for (int k = 0; k < 3; k++) note(b, 0, nn[k]);
    run_voice(b, 220.0, 2.0, &ph);
    printf("released: rms %.5f\n", rms());
    assert(rms() < 0.002);

    printf("test_chord: ok\n");
    return 0;
}
