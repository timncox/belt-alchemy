/* chord_src -- see chord_src.h. */
#include "chord_src.h"

#include <math.h>

/* ---- held notes ------------------------------------------------------- */

static bool has(const uint8_t *a, int n, uint8_t v)
{
    for (int i = 0; i < n; i++)
        if (a[i] == v) return true;
    return false;
}

void cs_hold(cs_held_t *h, const uint8_t *notes, int n, uint8_t vel,
             cs_emit_fn emit, void *ctx)
{
    uint8_t want[CS_MAX_NOTES];
    int     nw = 0;
    for (int i = 0; i < n && nw < CS_MAX_NOTES; i++)
        if (notes[i] < 128 && !has(want, nw, notes[i])) want[nw++] = notes[i];

    for (int i = 0; i < h->n; i++)
        if (!has(want, nw, h->note[i])) emit(0x80, h->note[i], 0, ctx);
    for (int i = 0; i < nw; i++)
        if (!has(h->note, h->n, want[i])) emit(0x90, want[i], vel, ctx);

    for (int i = 0; i < nw; i++) h->note[i] = want[i];
    h->n = (uint8_t)nw;
}

void cs_release(cs_held_t *h, cs_emit_fn emit, void *ctx)
{
    for (int i = 0; i < h->n; i++) emit(0x80, h->note[i], 0, ctx);
    h->n = 0;
}

/* ---- the sequencer's chords ------------------------------------------ */

int cs_seq_scale(int belt_scale)
{
    /* belt_core.c: Chromatic Major Minor HarmMinor Dorian Mixolydian
     * MajPent MinPent Blues */
    static const uint8_t kMap[9] = {
        SEQ_SCALE_MAJOR, SEQ_SCALE_MAJOR, SEQ_SCALE_MINOR,
        SEQ_SCALE_HARMONIC_MINOR, SEQ_SCALE_DORIAN, SEQ_SCALE_MIXOLYDIAN,
        SEQ_SCALE_MAJOR_PENT, SEQ_SCALE_MINOR_PENT, SEQ_SCALE_MINOR_PENT,
    };
    return (belt_scale >= 0 && belt_scale < 9) ? kMap[belt_scale] : SEQ_SCALE_MAJOR;
}

int cs_chord_notes(const seq_prog_t *pr, int ci, int base, uint8_t *out)
{
    if (ci < 0 || ci >= pr->count || ci >= SEQ_MAX_CHORDS) return 0;
    return cs_degree_notes(pr->root, pr->scale, pr->tones, pr->chord[ci].degree, base, out);
}

int cs_degree_notes(int key, int scale, int tones, int degree, int base, uint8_t *out)
{
    if (tones < 1) tones = 3;
    if (tones > CS_MAX_NOTES) tones = CS_MAX_NOTES;
    for (int i = 0; i < tones; i++)
    {
        int n = base + key + seq_degree_semis(scale, degree + 2 * i);
        while (n > 127) n -= 12;
        while (n < 0) n += 12;
        out[i] = (uint8_t)n;
    }
    return tones;
}

/* ---- CV pitch inputs ---------------------------------------------------- */

/* ~5 ms at the 1 ms poll: long enough to sit through ADC noise, short
 * against a chord change. */
#define CV_ALPHA 0.2f

/* ---- one-jack CV chord -------------------------------------------------- */

static float deg_pitch(int key, int scale, int d)   /* semitones above 0 V's C */
{
    return (float)(key + seq_degree_semis(scale, d));
}

/* The scale degree whose note is nearest semitone position s. */
static int nearest_degree(float s, int key, int scale)
{
    const int len = seq_scale_len(scale);
    int       d0  = (int)floorf((s - (float)key) / 12.0f * (float)len);
    int       best = d0;
    float     bd   = 1e9f;
    for (int d = d0 - 2; d <= d0 + 2; d++)
    {
        const float e = fabsf(s - deg_pitch(key, scale, d));
        if (e < bd) { bd = e; best = d; }
    }
    return best;
}

void cs_cvdeg_init(cs_cvdeg_t *c)
{
    c->smooth = 0.0f;
    c->degree = c->cand = 0;
    c->still  = 0;
    c->key = c->scale = -1;
    c->primed = false;
}

bool cs_cvdeg_poll(cs_cvdeg_t *c, float volts, int key, int scale)
{
    c->smooth = c->primed ? c->smooth + CV_ALPHA * (volts - c->smooth) : volts;
    const float s = c->smooth * 12.0f;
    const int   n = nearest_degree(s, key, scale);
    if (!c->primed || key != c->key || scale != c->scale)
    {
        /* first poll, or the scale under it moved: snap now */
        const bool moved = !c->primed || n != c->degree;
        c->primed = true;
        c->key    = key;
        c->scale  = scale;
        c->degree = c->cand = n;
        c->still  = 0;
        return moved;
    }
    if (n == c->degree
        || fabsf(s - deg_pitch(key, scale, c->degree)) - fabsf(s - deg_pitch(key, scale, n))
               <= 2.0f * CS_CV_HYST
        || nearest_degree(volts * 12.0f, key, scale) != n)
    {
        c->still = 0;   /* home, near a boundary, or passing through */
        return false;
    }
    if (n != c->cand) { c->cand = n; c->still = 0; }
    if (++c->still >= CS_CV_STILL_MS)
    {
        c->degree = n;
        c->still  = 0;
        return true;
    }
    return false;
}

void cs_cv_init(cs_cv_t *c)
{
    for (int i = 0; i < CS_CV_INPUTS; i++)
    {
        c->smooth[i] = 0.0f;
        c->semis[i]  = 0;
        c->cand[i]   = 0;
        c->still[i]  = 0;
    }
    c->primed = false;
}

bool cs_cv_poll(cs_cv_t *c, const float *volts, int n)
{
    if (n > CS_CV_INPUTS) n = CS_CV_INPUTS;
    bool changed = false;
    for (int i = 0; i < n; i++)
    {
        if (!c->primed) c->smooth[i] = volts[i];
        else c->smooth[i] += CV_ALPHA * (volts[i] - c->smooth[i]);

        const float s = c->smooth[i] * 12.0f;
        const int   r = (int)lroundf(s);
        if (!c->primed)
        {
            c->semis[i] = (int16_t)r;
            c->cand[i]  = (int16_t)r;
            changed     = true;
            continue;
        }
        /* A Schmitt trigger around the note held: it lets go only once the
         * pitch is half a semitone plus CS_CV_HYST away, so an input parked
         * on a boundary, noise and all, stays put -- while a sender off by
         * up to the whole rounding margin (+-41.7 mV) still lands on its
         * nearest note. The input itself must round to the same note as the
         * smoothed value: on its way through a jump of a fifth the smoothed
         * value passes the semitones between, and those must not count. */
        const float raw = volts[i] * 12.0f;
        if (fabsf(s - (float)c->semis[i]) <= 0.5f + CS_CV_HYST
            || (int)lroundf(raw) != r)
        {
            c->still[i] = 0;   /* home, near the boundary, or passing */
            continue;
        }
        if (r != c->cand[i])
        {
            c->cand[i]  = (int16_t)r;
            c->still[i] = 0;
        }
        if (++c->still[i] >= CS_CV_STILL_MS)
        {
            c->semis[i] = (int16_t)r;
            c->still[i] = 0;
            changed     = true;
        }
    }
    c->primed = true;
    return changed;
}

int cs_cv_notes(const cs_cv_t *c, int n, int base, uint8_t *out)
{
    if (n > CS_CV_INPUTS) n = CS_CV_INPUTS;
    for (int i = 0; i < n; i++)
    {
        int v = base + c->semis[i];
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        out[i] = (uint8_t)v;
    }
    return n;
}

/* ---- calibration ---------------------------------------------------------- */

const float cs_cal_ref[CS_CV_INPUTS] = {0.0f, 4.0f / 12.0f, 7.0f / 12.0f, 1.0f};

unsigned cs_cal_learn(const float *measured, int n, int16_t *offset_mv)
{
    unsigned ok = 0;
    if (n > CS_CV_INPUTS) n = CS_CV_INPUTS;
    for (int i = 0; i < n; i++)
    {
        const float off = measured[i] - cs_cal_ref[i];
        if (!(fabsf(off) <= CS_CAL_MAX_V)) continue;
        offset_mv[i] = (int16_t)lroundf(off * 1000.0f);
        ok |= 1u << i;
    }
    return ok;
}
