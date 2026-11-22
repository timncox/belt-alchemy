/* seq -- see seq.h for the rules this file keeps. */
#include "seq.h"

#include <math.h>
#include <string.h>

#define START_EPS 1e-6    /* the transport starts this far before tick 0 so
                             that step one falls inside the first window */
#define RETRIG_US 2000    /* gate dip between two separate notes */
#define MAX_SEMIS 60      /* 5 V: notes above fold down an octave */

/* ---------------------------------------------------------------- scales */

static const int8_t kScales[SEQ_SCALE_COUNT][7] = {
    {0, 2, 4, 5, 7, 9, 11}, /* major */
    {0, 2, 3, 5, 7, 8, 10}, /* natural minor */
    {0, 2, 3, 5, 7, 9, 10}, /* dorian */
    {0, 1, 3, 5, 7, 8, 10}, /* phrygian */
    {0, 2, 4, 6, 7, 9, 11}, /* lydian */
    {0, 2, 4, 5, 7, 9, 10}, /* mixolydian */
    {0, 2, 3, 5, 7, 8, 11}, /* harmonic minor */
    {0, 3, 5, 7, 10},       /* minor pentatonic */
    {0, 2, 4, 7, 9},        /* major pentatonic */
};
static const uint8_t kScaleLen[SEQ_SCALE_COUNT] = {7, 7, 7, 7, 7, 7, 7, 5, 5};

int seq_scale_len(int scale)
{
    if(scale < 0 || scale >= SEQ_SCALE_COUNT)
        scale = 0;
    return kScaleLen[scale];
}

int seq_degree_semis(int scale, int degree)
{
    if(scale < 0 || scale >= SEQ_SCALE_COUNT)
        scale = 0;
    int n   = kScaleLen[scale];
    int oct = degree >= 0 ? degree / n : -((-degree + n - 1) / n);
    int i   = degree - oct * n;
    return oct * 12 + kScales[scale][i];
}

/* ---------------------------------------------------------------- hashing */

enum { F_HIT = 1, F_LEN, F_TONE, F_IDX, F_SLIDE, F_JUMP, F_JSIGN };

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* A fixed number in [0, 1) for one property of one step. */
static float unit(uint32_t seed, uint32_t field)
{
    return (float)(mix32(seed ^ (field * 0x9e3779b9U)) >> 8) * (1.0f / 16777216.0f);
}

static uint32_t rng_next(seq_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->rng = x;
}

static float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

/* ---------------------------------------------------------------- voices */

static seq_pattern_t *cur_pat(seq_t *s, int v) { return &s->pat[v][s->v[v].slot]; }
static const seq_pattern_t *cur_pat_c(const seq_t *s, int v)
{
    return &s->pat[v][s->v[v].slot];
}

/* Downbeats come in first as DENSITY rises: a step's hit number is scaled
 * down on the beat and the half-beat, so it crosses the threshold sooner. */
static float beat_weight(int k)
{
    return (k % 4 == 0) ? 0.55f : (k % 2 == 0) ? 0.8f : 1.0f;
}

static seq_step_t realize(const seq_t *s, int v, const seq_pattern_t *p, int k)
{
    const seq_knobs_t *kn = &p->knobs;
    uint32_t           sd = p->seed[k];
    seq_step_t         st;
    memset(&st, 0, sizeof st);

    st.on = unit(sd, F_HIT) * beat_weight(k) < clamp01(kn->density);

    /* LENGTH bends a fixed number through a power curve: the exponent falls
     * as the knob rises, so every note only gets longer. */
    float g = exp2f((0.5f - clamp01(kn->length)) * 4.0f);
    float d = powf(unit(sd, F_LEN), g);
    int   len = (int)(2.0f + d * 30.0f + 0.5f);
    st.len  = (uint8_t)(len > 32 ? 32 : len);

    st.scale = unit(sd, F_TONE) < clamp01(kn->tone_bias);
    int range = kn->range < 1 ? 1 : (kn->range > 3 ? 3 : kn->range);
    int n = (st.scale ? seq_scale_len(s->prog.scale) : s->prog.tones) * range;
    float u = unit(sd, F_IDX);
    if(v == SEQ_BASS)
        u = u * u; /* the bass leans on the root */
    int idx = (int)(u * (float)n);
    st.index = (uint8_t)(idx >= n ? n - 1 : idx);

    st.slide = unit(sd, F_SLIDE) < clamp01(kn->slide);
    if(unit(sd, F_JUMP) < clamp01(kn->jump))
        st.jump = (v == SEQ_BASS || unit(sd, F_JSIGN) < 0.5f) ? 1 : -1;
    return st;
}

seq_knobs_t *seq_knobs(seq_t *s, int v) { return &cur_pat(s, v)->knobs; }

seq_step_t seq_step(const seq_t *s, int v, int k)
{
    const seq_pattern_t *p = cur_pat_c(s, v);
    k %= SEQ_MAX_STEPS;
    if(k < 0)
        k += SEQ_MAX_STEPS;
    return p->locked ? p->lock[k] : realize(s, v, p, k);
}

static void save_undo(seq_pattern_t *p)
{
    memcpy(p->undo, p->seed, sizeof p->seed);
    p->has_undo = true;
}

void seq_roll(seq_t *s, int v)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(p->locked)
        return; /* LOCK protects the keeper */
    save_undo(p);
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        p->seed[k] = rng_next(s);
}

void seq_mutate(seq_t *s, int v, float amount)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(p->locked)
        return;
    save_undo(p);
    uint32_t th = (uint32_t)(clamp01(amount) * 4294967295.0);
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
    {
        uint32_t r = rng_next(s);
        if(amount >= 1.0f || r < th)
            p->seed[k] = rng_next(s);
    }
}

void seq_undo(seq_t *s, int v)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(!p->has_undo || p->locked)
        return;
    uint32_t tmp[SEQ_MAX_STEPS];
    memcpy(tmp, p->seed, sizeof tmp);
    memcpy(p->seed, p->undo, sizeof tmp);
    memcpy(p->undo, tmp, sizeof tmp);
}

void seq_lock(seq_t *s, int v, bool lock)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(lock && !p->locked)
        for(int k = 0; k < SEQ_MAX_STEPS; k++)
            p->lock[k] = realize(s, v, p, k);
    p->locked = lock;
}

/* ---------------------------------------------------------------- chords */

static double prog_ticks(const seq_prog_t *pr)
{
    double t = 0;
    for(int i = 0; i < pr->count; i++)
        t += (double)(pr->chord[i].bars ? pr->chord[i].bars : 1) * SEQ_BAR;
    return t;
}

static int chord_at(const seq_prog_t *pr, double tick)
{
    double total = prog_ticks(pr);
    if(total <= 0 || tick < 0)
        return 0;
    double t = fmod(tick, total);
    for(int i = 0; i < pr->count; i++)
    {
        double len = (double)(pr->chord[i].bars ? pr->chord[i].bars : 1) * SEQ_BAR;
        if(t < len)
            return i;
        t -= len;
    }
    return pr->count - 1;
}

int seq_chord_index(const seq_t *s) { return chord_at(&s->prog, s->pos); }

/* Semitones above the voice's 0 V for step st under chord ci. */
static float note_for(const seq_t *s, int v, seq_step_t st, int ci)
{
    const seq_prog_t *pr = &s->prog;
    int n   = seq_scale_len(pr->scale);
    int deg = pr->chord[ci].degree;
    if(v == SEQ_BASS && deg > n / 2)
        deg -= n; /* keep the bass near the key's root: V goes below, not above */
    int tones = pr->tones < 1 ? 3 : pr->tones;
    if(st.scale)
        deg += st.index;
    else
        deg += 2 * (st.index % tones) + n * (st.index / tones);
    int semis = pr->root + seq_degree_semis(pr->scale, deg)
                + 12 * (s->base_octave[v] + cur_pat_c(s, v)->knobs.octave + st.jump);
    while(semis > MAX_SEMIS)
        semis -= 12;
    while(semis < 0)
        semis += 12;
    return (float)semis;
}

float seq_step_note(const seq_t *s, int v, int k)
{
    return note_for(s, v, seq_step(s, v, k), seq_chord_index(s));
}

/* ---------------------------------------------------------------- patterns */

void seq_voice_queue(seq_t *s, int v, int slot)
{
    if(slot >= 0 && slot < SEQ_VOICE_SLOTS)
        s->v[v].queued = (uint8_t)slot;
}

void seq_voice_copy(seq_t *s, int v, int from, int to)
{
    if(from < 0 || to < 0 || from >= SEQ_VOICE_SLOTS || to >= SEQ_VOICE_SLOTS)
        return;
    s->pat[v][to] = s->pat[v][from];
}

/* ---------------------------------------------------------------- drum generator */

bool seq_euclid(int hits, int n, int rot, int k)
{
    if(n < 1 || hits < 1)
        return false;
    if(hits >= n)
        return true;
    int i = ((k - rot) % n + n) % n;
    /* Bresenham form: the same set as Bjorklund, first hit on step 0 */
    return (i * hits) % n < hits;
}

/* Per-drum rotations for EUCLID 1-4 (kick, snare, hat, perc), in 16ths:
 * snare 4 puts E(2,16) on 2 and 4, hat 2 puts E(4,16) on the off-beats. */
static const int8_t kEuclidRot[4][SEQ_DRUMS] = {
    {0, 4, 2, 3},
    {0, 4, 0, 6},
    {0, 2, 1, 5},
    {0, 6, 3, 1},
};

/* Where each drum's steps sit on the amount knob: a step of a class plays
 * once the amount passes lo + u * span, u being its fixed number. The
 * bands overlap a little, so the beat fills first, then the 8ths, then
 * the 16ths, and lo + span <= 1 so a full knob fills the bar. */
static void drum_band(int d, int k, float *lo, float *span)
{
    static const float kBand[SEQ_DRUMS][3][2] = {
        /* strong        medium        rest */
        {{0.f, .30f}, {.25f, .40f}, {.45f, .55f}}, /* kick: beats, 8ths, 16ths */
        {{0.f, .25f}, {.30f, .50f}, {.45f, .55f}}, /* snare: 2 and 4, 8ths, 16ths */
        {{0.f, .30f}, {.20f, .40f}, {.40f, .60f}}, /* hat: off-beats, 8ths, 16ths */
        {{0.f, 1.f}, {0.f, 1.f}, {0.f, 1.f}},      /* perc: anywhere */
    };
    int cls;
    switch(d)
    {
        case SEQ_KICK: cls = k % 4 == 0 ? 0 : (k % 2 == 0 ? 1 : 2); break;
        case SEQ_SNARE: cls = k % 8 == 4 ? 0 : (k % 2 == 0 ? 1 : 2); break;
        case SEQ_HAT: cls = k % 4 == 2 ? 0 : (k % 2 == 0 ? 1 : 2); break;
        default: cls = 0; break;
    }
    *lo   = kBand[d][cls][0];
    *span = kBand[d][cls][1];
}

static bool drum_generated(const seq_drums_t *dr, int d, int k)
{
    const float amt = clamp01(dr->gen[d]);
    if(amt <= 0.f)
        return false;
    const int style = dr->style % SEQ_DSTYLE_COUNT;
    const int n     = dr->steps[d] ? dr->steps[d] : 16;
    if(style >= SEQ_DSTYLE_EUCLID1)
    {
        const int hits = (int)(amt * (float)n + 0.5f);
        return seq_euclid(hits, n, kEuclidRot[style - SEQ_DSTYLE_EUCLID1][d], k);
    }
    const uint32_t seed = mix32(0xD5A10000u + (uint32_t)style * 0x10000u
                                + (uint32_t)d * 0x100u + (uint32_t)k);
    float lo, span;
    drum_band(d, k % 16, &lo, &span);
    return lo + unit(seed, F_HIT) * span < amt;
}

seq_drums_t *seq_drums(seq_t *s) { return &s->drums[s->drum_slot]; }

int seq_drum_prob(const seq_t *s, int d, int k, bool *generated)
{
    if(generated)
        *generated = false;
    if(d < 0 || d >= SEQ_DRUMS || k < 0 || k >= SEQ_MAX_STEPS)
        return 0;
    const seq_drums_t *dr = &s->drums[s->drum_slot];
    const int          p  = dr->prob[d][k];
    if(p < 4 && drum_generated(dr, d, k))
    {
        if(generated)
            *generated = p == 0;
        return 4;
    }
    return p;
}

/* ---------------------------------------------------------------- drum text */

/* Case-insensitive: is the n-character word at w exactly kw? */
static bool word_is(const char *w, int n, const char *kw)
{
    int i = 0;
    while(i < n && kw[i] && (w[i] | 0x20) == kw[i])
        i++;
    return i == n && kw[i] == 0;
}

static int drum_name(const char *w, int n)
{
    static const char *const kNames[SEQ_DRUMS] = {"kick", "snare", "hat", "perc"};
    for(int d = 0; d < SEQ_DRUMS; d++)
        if(word_is(w, n, kNames[d]))
            return d;
    return -1;
}

/* One pass; with dst NULL it only checks. */
static int drums_pass(seq_drums_t *dst, const char *t, size_t len, bool *named)
{
    int    slot = 0, written = 0;
    bool   any_line = false;
    size_t i = 0;
    while(i < len)
    {
        size_t e = i;
        while(e < len && t[e] != '\n' && t[e] != '\r')
            e++;
        /* trim, drop a comment */
        size_t a = i, b = e;
        for(size_t j = a; j < b; j++)
            if(t[j] == '#')
            {
                b = j;
                break;
            }
        while(a < b && (t[a] == ' ' || t[a] == '\t'))
            a++;
        while(b > a && (t[b - 1] == ' ' || t[b - 1] == '\t'))
            b--;
        i = e + 1;
        if(a == b)
            continue;
        size_t w = a;
        while(w < b && t[w] != ' ' && t[w] != '\t')
            w++;
        const int wl = (int)(w - a);
        if(word_is(t + a, wl, "pattern"))
        {
            /* "pattern N" */
            int n = 0, digits = 0;
            for(size_t j = w; j < b; j++)
            {
                if(t[j] == ' ' || t[j] == '\t')
                    continue;
                if(t[j] < '0' || t[j] > '9' || digits > 2)
                    return -1;
                n = n * 10 + (t[j] - '0');
                digits++;
            }
            if(!digits || n < 1 || n > SEQ_DRUM_SLOTS)
                return -1;
            slot = n - 1;
            continue;
        }
        const int d = drum_name(t + a, wl);
        if(d < 0)
            return -1;
        uint8_t prob[SEQ_MAX_STEPS];
        int     n = 0;
        for(size_t j = w; j < b; j++)
        {
            const char ch = t[j];
            int        p;
            if(ch == ' ' || ch == '\t' || ch == '|')
                continue;
            if(ch == 'x' || ch == 'X') p = 4;
            else if(ch >= '1' && ch <= '4') p = ch - '0';
            else if(ch == '.' || ch == '-' || ch == '0') p = 0;
            else return -1;
            if(n >= SEQ_MAX_STEPS)
                return -1;
            prob[n++] = (uint8_t)p;
        }
        if(n == 0)
            return -1;
        any_line = true;
        if(!named[slot])
        {
            named[slot] = true;
            written++;
            if(dst)
            {
                dst[slot].gen[0] = dst[slot].gen[1] = dst[slot].gen[2] = dst[slot].gen[3] = 0.f;
            }
        }
        if(dst)
        {
            memset(dst[slot].prob[d], 0, sizeof dst[slot].prob[d]);
            memcpy(dst[slot].prob[d], prob, (size_t)n);
            dst[slot].steps[d] = (uint8_t)n;
        }
    }
    return any_line ? written : 0;
}

int seq_drums_parse(seq_t *s, const char *text, size_t len)
{
    bool named[SEQ_DRUM_SLOTS] = {false};
    if(drums_pass(NULL, text, len, named) < 0)
        return -1;
    memset(named, 0, sizeof named);
    return drums_pass(s->drums, text, len, named);
}

void seq_drum_queue(seq_t *s, int slot)
{
    if(slot >= 0 && slot < SEQ_DRUM_SLOTS)
        s->drum_queued = (uint8_t)slot;
}

void seq_voice_mute(seq_t *s, int v, bool mute) { s->v[v].mute = mute; }
void seq_drum_mute(seq_t *s, int d, bool mute) { s->drum_mute[d] = mute; }

void seq_drum_set(seq_t *s, int d, int k, int prob)
{
    if(d < 0 || d >= SEQ_DRUMS || k < 0 || k >= SEQ_MAX_STEPS)
        return;
    s->drums[s->drum_slot].prob[d][k] = (uint8_t)(prob < 0 ? 0 : (prob > 4 ? 4 : prob));
}

/* ---------------------------------------------------------------- init */

static void default_drums(seq_drums_t *dr)
{
    memset(dr, 0, sizeof *dr);
    for(int d = 0; d < SEQ_DRUMS; d++)
        dr->steps[d] = 16;
    for(int k = 0; k < 16; k += 4)
        dr->prob[SEQ_KICK][k] = 4;
    dr->prob[SEQ_SNARE][4]  = 4;
    dr->prob[SEQ_SNARE][12] = 4;
    for(int k = 2; k < 16; k += 4)
        dr->prob[SEQ_HAT][k] = 4;
}

void seq_init(seq_t *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->rng = mix32(seed) | 1u;

    /* A minor, i - VI - III - VII, a bar each. */
    s->prog.root  = 9;
    s->prog.scale = SEQ_SCALE_MINOR;
    s->prog.tones = 3;
    s->prog.count = 4;
    const int8_t degs[4] = {0, 5, 2, 6};
    for(int i = 0; i < SEQ_MAX_CHORDS; i++) /* spare slots valid too */
    {
        s->prog.chord[i].degree = i < 4 ? degs[i] : 0;
        s->prog.chord[i].bars   = 1;
    }

    s->base_octave[SEQ_BASS] = 1;
    s->base_octave[SEQ_LEAD] = 2;

    const seq_knobs_t kb = {.density = 0.6f, .length = 0.4f, .slide = 0.2f,
                            .jump = 0.15f, .tone_bias = 0.1f, .swing = 1.0f,
                            .octave = 0, .range = 1, .steps = 16, .div = 1};
    const seq_knobs_t kl = {.density = 0.35f, .length = 0.6f, .slide = 0.1f,
                            .jump = 0.1f, .tone_bias = 0.3f, .swing = 1.0f,
                            .octave = 0, .range = 2, .steps = 16, .div = 1};
    for(int v = 0; v < SEQ_VOICES; v++)
        for(int sl = 0; sl < SEQ_VOICE_SLOTS; sl++)
        {
            seq_pattern_t *p = &s->pat[v][sl];
            p->knobs = v == SEQ_BASS ? kb : kl;
            for(int k = 0; k < SEQ_MAX_STEPS; k++)
                p->seed[k] = rng_next(s);
        }
    for(int sl = 0; sl < SEQ_DRUM_SLOTS; sl++)
        default_drums(&s->drums[sl]);

    s->bpm        = 120.0f;
    s->swing      = 0.5f;
    s->glide_ms   = 60;
    s->clock_ppqn = 4;
    for(int v = 0; v < SEQ_VOICES; v++)
        s->v[v].cur_step = -1;
    for(int d = 0; d < SEQ_DRUMS; d++)
        s->drum_step[d] = -1;
}

/* ---------------------------------------------------------------- transport */

static void all_gates_off(seq_t *s)
{
    for(int v = 0; v < SEQ_VOICES; v++)
        s->v[v].gate = false;
}

static void rewind(seq_t *s)
{
    s->pos = -START_EPS;
    for(int v = 0; v < SEQ_VOICES; v++)
        s->v[v].cur_step = -1;
    for(int d = 0; d < SEQ_DRUMS; d++)
        s->drum_step[d] = -1;
}

void seq_play(seq_t *s)
{
    s->hold = false;
    if(s->ext)
    {
        /* Following J1: step one plays on the next edge, in time. */
        s->reset_pending = true;
        return;
    }
    rewind(s);
    s->running = true;
}

static void halt(seq_t *s)
{
    s->running = false;
    all_gates_off(s);
}

void seq_stop(seq_t *s)
{
    s->hold = true; /* clock edges no longer start it; seq_play() does */
    halt(s);
}

static uint32_t ticks_per_edge(const seq_t *s)
{
    uint32_t p = s->clock_ppqn ? s->clock_ppqn : 4;
    return SEQ_PPQN / p;
}

void seq_clock_edge(seq_t *s, uint32_t now_us)
{
    if(!s->ext)
    {
        /* First edge: the external clock takes over and step one plays now
         * (unless the player stopped it: then it only keeps time). */
        s->ext       = true;
        s->running   = !s->hold;
        s->edges     = 0;
        s->period_us = 0;
        s->edge_us   = now_us;
        s->reset_pending = false;
        rewind(s);
        return;
    }
    uint32_t p = now_us - s->edge_us;
    s->period_us = s->period_us ? (s->period_us + p) / 2 : p;
    s->edge_us   = now_us;
    if(s->hold)
        return;
    s->running = true;
    if(s->reset_pending)
    {
        s->reset_pending = false;
        s->edges         = 0;
        rewind(s);
        return;
    }
    s->edges++;
}

void seq_reset_edge(seq_t *s, uint32_t now_us)
{
    (void)now_us;
    if(s->ext)
        s->reset_pending = true;
    else
        rewind(s);
}

/* ---------------------------------------------------------------- playback */

static void start_note(seq_t *s, int v, long g, double start, uint32_t now_us)
{
    seq_vstate_t        *vs = &s->v[v];
    const seq_pattern_t *p  = cur_pat_c(s, v);
    int steps = p->knobs.steps ? p->knobs.steps : 16;
    int k     = (int)(g % steps);
    vs->cur_step = k;

    seq_step_t st = seq_step(s, v, k);
    if(vs->mute || !st.on)
        return; /* a tied note before keeps sounding until its gate_off */

    int    div  = p->knobs.div ? p->knobs.div : 1;
    double tpsv = (double)SEQ_TPS * div;
    float  note = note_for(s, v, st, chord_at(&s->prog, start));
    /* Legato only when the note before was a real tie (longer than its step);
     * a full-step note whose gate merely overlaps because of swing is not. */
    bool   holding = vs->gate && vs->tied && vs->gate_off > start;

    if(st.slide && vs->gate)
    {
        /* 303 slide: the gate carries over and the pitch glides. */
        vs->target = note;
        float dist = fabsf(note - vs->pitch);
        vs->glide_rate = s->glide_ms ? dist / (float)s->glide_ms : 0.f;
        if(vs->glide_rate == 0.f)
            vs->pitch = note;
    }
    else
    {
        vs->pitch = vs->target = note;
        vs->glide_rate = 0.f;
        if(vs->gate && !holding)
        {
            /* The last note only just ended: dip so this one is heard as new.
             * A note still held (a tie) goes on legato with no new attack. */
            vs->dip       = true;
            vs->dip_until = now_us + RETRIG_US;
        }
    }
    vs->gate     = true;
    vs->tied     = st.len > 16;
    vs->gate_off = start + tpsv * st.len / 16.0;

    /* A slide on the NEXT step holds this gate into it, as on a 303: the
     * next note then finds the gate still up and glides legato. 1.5 steps
     * covers the next step's swing delay. */
    seq_step_t nx = seq_step(s, v, (k + 1) % steps);
    if(nx.on && nx.slide && !vs->mute && vs->gate_off < start + tpsv * 1.5)
        vs->gate_off = start + tpsv * 1.5;
}

static void fire_drum(seq_t *s, int d, long g, uint32_t now_us)
{
    const seq_drums_t *dr = &s->drums[s->drum_slot];
    int steps = dr->steps[d] ? dr->steps[d] : 16;
    int k     = (int)(g % steps);
    s->drum_step[d] = k;
    int p = seq_drum_prob(s, d, k, NULL);
    if(p == 0 || s->drum_mute[d])
        return;
    if(p < 4 && (int)(rng_next(s) % 4) >= p)
        return;
    s->trig_on[d]    = true;
    s->trig_until[d] = now_us + SEQ_TRIG_MS * 1000u;
}

static double swing_off(const seq_t *s, long g, double tps, float amount)
{
    if((g & 1) == 0)
        return 0.0;
    double sw = (double)s->swing - 0.5;
    if(sw < 0)
        sw = 0;
    if(sw > 0.25)
        sw = 0.25;
    return sw * 2.0 * tps * clamp01(amount);
}

/* Fire everything that starts in [a, b). */
static void run_window(seq_t *s, double a, double b, uint32_t now_us)
{
    /* Bar lines first: queued patterns change there. */
    for(long bar = (long)floor(a / SEQ_BAR); (double)bar * SEQ_BAR < b; bar++)
    {
        double t = (double)bar * SEQ_BAR;
        if(bar < 0 || t < a)
            continue;
        for(int v = 0; v < SEQ_VOICES; v++)
            s->v[v].slot = s->v[v].queued;
        s->drum_slot = s->drum_queued;
    }

    for(int v = 0; v < SEQ_VOICES; v++)
    {
        const seq_pattern_t *p = cur_pat_c(s, v);
        int    div = p->knobs.div ? p->knobs.div : 1;
        double tps = (double)SEQ_TPS * div;
        for(long g = (long)floor(a / tps) - 1; (double)g * tps < b; g++)
        {
            if(g < 0)
                continue;
            double start = (double)g * tps + swing_off(s, g, tps, p->knobs.swing);
            if(start >= a && start < b)
                start_note(s, v, g, start, now_us);
        }
    }

    for(long g = (long)floor(a / SEQ_TPS) - 1; (double)g * SEQ_TPS < b; g++)
    {
        if(g < 0)
            continue;
        double start = (double)g * SEQ_TPS + swing_off(s, g, SEQ_TPS, 1.0f);
        if(start >= a && start < b)
            for(int d = 0; d < SEQ_DRUMS; d++)
                fire_drum(s, d, g, now_us);
    }
}

void seq_process(seq_t *s, uint32_t now_us, seq_out_t *out)
{
    uint32_t dt_us = s->have_last ? now_us - s->last_us : 0;
    s->last_us   = now_us;
    s->have_last = true;

    if(s->ext && now_us - s->edge_us > SEQ_CLOCK_LOST_MS * 1000u)
    {
        s->ext = false;
        halt(s); /* not a player's stop: the next clock starts it again */
    }

    if(s->running)
    {
        double next;
        if(s->ext)
        {
            /* Extrapolate from the last edge at the measured period, but
             * never past where the next edge will land; the tick of the
             * latest edge itself is always reached. */
            double tpe  = (double)ticks_per_edge(s);
            double base = (double)s->edges * tpe;
            next = base + START_EPS;
            if(s->period_us)
            {
                uint32_t since = now_us - s->edge_us;
                if(since > s->period_us)
                    since = s->period_us;
                double x = base + tpe * (double)since / (double)s->period_us;
                if(x > next)
                    next = x;
            }
            if(next > base + tpe - START_EPS)
                next = base + tpe - START_EPS;
        }
        else
            next = s->pos + (double)dt_us * s->bpm * SEQ_PPQN / 60e6;

        if(next > s->pos)
        {
            run_window(s, s->pos, next, now_us);
            s->pos = next;
        }

        for(int v = 0; v < SEQ_VOICES; v++)
            if(s->v[v].gate && s->pos >= s->v[v].gate_off)
                s->v[v].gate = false;
    }

    /* Glide and the retrigger dip run in real time. */
    float dt_ms = (float)dt_us / 1000.0f;
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        seq_vstate_t *vs = &s->v[v];
        if(vs->dip && (int32_t)(now_us - vs->dip_until) >= 0)
            vs->dip = false;
        if(vs->glide_rate > 0.f)
        {
            float step = vs->glide_rate * dt_ms;
            float diff = vs->target - vs->pitch;
            if(fabsf(diff) <= step)
            {
                vs->pitch      = vs->target;
                vs->glide_rate = 0.f;
            }
            else
                vs->pitch += diff > 0 ? step : -step;
        }
    }

    for(int d = 0; d < SEQ_DRUMS; d++)
        if(s->trig_on[d] && (int32_t)(now_us - s->trig_until[d]) >= 0)
            s->trig_on[d] = false;

    if(out)
    {
        for(int v = 0; v < SEQ_VOICES; v++)
        {
            const seq_vstate_t *vs = &s->v[v];
            out->pitch[v] = vs->pitch / 12.0f;
            out->gate[v]  = vs->gate && !vs->dip;
        }
        for(int d = 0; d < SEQ_DRUMS; d++)
            out->trig[d] = s->trig_on[d];
    }
}

/* ---------------------------------------------------------------- persistence */

typedef struct {
    uint8_t       *w;  /* writing, or NULL */
    const uint8_t *r;  /* reading, or NULL */
    size_t         n;  /* bytes so far */
} cur_t;

static void io(cur_t *c, void *p, size_t len)
{
    if(c->n + len > SEQ_STATE_BYTES) /* never: the tests pin the layout */
        return;
    if(c->w) memcpy(c->w + c->n, p, len);
    else memcpy(p, c->r + c->n, len);
    c->n += len;
}

static float fclamp(float x, float lo, float hi)
{
    if(!(x >= lo)) return lo; /* NaN lands here too */
    return x > hi ? hi : x;
}

static int iclamp(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* One pass over every kept field. On a write the fields are copied out of
 * s; on a read into it, and clamped afterwards by state_sanitize(). */
static size_t state_io(seq_t *s, cur_t *c)
{
    uint8_t ver = SEQ_STATE_VERSION;
    io(c, &ver, 1);
    for(int v = 0; v < SEQ_VOICES; v++)
        for(int sl = 0; sl < SEQ_VOICE_SLOTS; sl++)
        {
            seq_pattern_t *p = &s->pat[v][sl];
            seq_knobs_t   *k = &p->knobs;
            io(c, p->seed, sizeof p->seed);
            io(c, &k->density, 4);
            io(c, &k->length, 4);
            io(c, &k->slide, 4);
            io(c, &k->jump, 4);
            io(c, &k->tone_bias, 4);
            io(c, &k->swing, 4);
            io(c, &k->octave, 1);
            io(c, &k->range, 1);
            io(c, &k->steps, 1);
            io(c, &k->div, 1);
            io(c, &p->locked, 1);
            uint8_t locked; /* raw: a read has not been fixed up yet */
            memcpy(&locked, &p->locked, 1);
            if(c->w && !locked)
            {
                /* seq_lock() refills the steps when it locks; an unlocked
                 * pattern's leftovers are written as zeros */
                c->n += SEQ_MAX_STEPS * 6;
                continue;
            }
            for(int i = 0; i < SEQ_MAX_STEPS; i++)
            {
                seq_step_t *st = &p->lock[i];
                io(c, &st->on, 1);
                io(c, &st->slide, 1);
                io(c, &st->scale, 1);
                io(c, &st->jump, 1);
                io(c, &st->index, 1);
                io(c, &st->len, 1);
            }
        }
    for(int sl = 0; sl < SEQ_DRUM_SLOTS; sl++)
    {
        io(c, s->drums[sl].prob, sizeof s->drums[sl].prob);
        io(c, s->drums[sl].steps, sizeof s->drums[sl].steps);
        for(int d = 0; d < SEQ_DRUMS; d++)
            io(c, &s->drums[sl].gen[d], 4);
        io(c, &s->drums[sl].style, 1);
    }
    io(c, &s->prog.root, 1);
    io(c, &s->prog.scale, 1);
    io(c, &s->prog.tones, 1);
    io(c, &s->prog.count, 1);
    for(int i = 0; i < SEQ_MAX_CHORDS; i++)
    {
        io(c, &s->prog.chord[i].degree, 1);
        io(c, &s->prog.chord[i].bars, 1);
    }
    io(c, s->base_octave, sizeof s->base_octave);
    io(c, &s->bpm, 4);
    io(c, &s->swing, 4);
    io(c, &s->glide_ms, 2);
    io(c, &s->clock_ppqn, 1);
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        io(c, &s->v[v].slot, 1);
        io(c, &s->v[v].mute, 1);
    }
    io(c, &s->drum_slot, 1);
    io(c, s->drum_mute, sizeof s->drum_mute);
    io(c, &s->drums_file_crc, 4);
    return c->n;
}

/* bools read from bytes: anything but 0/1 is not a valid _Bool */
static void fix_bool(bool *b)
{
    uint8_t x;
    memcpy(&x, b, 1);
    *b = x != 0;
}

static void state_sanitize(seq_t *s)
{
    for(int v = 0; v < SEQ_VOICES; v++)
        for(int sl = 0; sl < SEQ_VOICE_SLOTS; sl++)
        {
            seq_pattern_t *p = &s->pat[v][sl];
            seq_knobs_t   *k = &p->knobs;
            k->density   = fclamp(k->density, 0.f, 1.f);
            k->length    = fclamp(k->length, 0.f, 1.f);
            k->slide     = fclamp(k->slide, 0.f, 1.f);
            k->jump      = fclamp(k->jump, 0.f, 1.f);
            k->tone_bias = fclamp(k->tone_bias, 0.f, 1.f);
            k->swing     = fclamp(k->swing, 0.f, 1.f);
            k->octave    = (int8_t)iclamp(k->octave, -3, 3);
            k->range     = (uint8_t)iclamp(k->range, 1, 3);
            k->steps     = (uint8_t)iclamp(k->steps, 1, SEQ_MAX_STEPS);
            k->div       = (uint8_t)iclamp(k->div, 1, 4);
            fix_bool(&p->locked);
            p->has_undo = false;
            if(!p->locked) /* seq_lock() refills it; keep the blob stable */
                memset(p->lock, 0, sizeof p->lock);
            else
            for(int i = 0; i < SEQ_MAX_STEPS; i++)
            {
                seq_step_t *st = &p->lock[i];
                fix_bool(&st->on);
                fix_bool(&st->slide);
                fix_bool(&st->scale);
                st->jump  = (int8_t)iclamp(st->jump, -1, 1);
                st->index = (uint8_t)iclamp(st->index, 0, 15);
                st->len   = (uint8_t)iclamp(st->len, 2, 32);
            }
        }
    for(int sl = 0; sl < SEQ_DRUM_SLOTS; sl++)
        for(int d = 0; d < SEQ_DRUMS; d++)
        {
            for(int i = 0; i < SEQ_MAX_STEPS; i++)
                if(s->drums[sl].prob[d][i] > 4) s->drums[sl].prob[d][i] = 4;
            s->drums[sl].steps[d] = (uint8_t)iclamp(s->drums[sl].steps[d], 1, SEQ_MAX_STEPS);
            s->drums[sl].gen[d]   = fclamp(s->drums[sl].gen[d], 0.f, 1.f);
        }
    for(int sl = 0; sl < SEQ_DRUM_SLOTS; sl++)
        s->drums[sl].style %= SEQ_DSTYLE_COUNT;
    s->prog.root  = (uint8_t)(s->prog.root % 12);
    s->prog.scale = (uint8_t)(s->prog.scale % SEQ_SCALE_COUNT);
    s->prog.tones = (uint8_t)iclamp(s->prog.tones, 3, 4);
    s->prog.count = (uint8_t)iclamp(s->prog.count, 1, SEQ_MAX_CHORDS);
    for(int i = 0; i < SEQ_MAX_CHORDS; i++)
    {
        s->prog.chord[i].degree = (int8_t)iclamp(s->prog.chord[i].degree, -7, 13);
        s->prog.chord[i].bars   = (uint8_t)iclamp(s->prog.chord[i].bars, 1, 8);
    }
    for(int v = 0; v < SEQ_VOICES; v++)
        s->base_octave[v] = (uint8_t)iclamp(s->base_octave[v], 0, 4);
    s->bpm      = fclamp(s->bpm, 30.f, 300.f);
    s->swing    = fclamp(s->swing, 0.5f, 0.75f);
    s->glide_ms = (uint16_t)iclamp(s->glide_ms, 0, 2000);
    if(s->clock_ppqn != 1 && s->clock_ppqn != 2 && s->clock_ppqn != 4 && s->clock_ppqn != 24)
        s->clock_ppqn = 4;
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        s->v[v].slot %= SEQ_VOICE_SLOTS;
        s->v[v].queued = s->v[v].slot;
        fix_bool(&s->v[v].mute);
    }
    s->drum_slot  %= SEQ_DRUM_SLOTS;
    s->drum_queued = s->drum_slot;
    for(int d = 0; d < SEQ_DRUMS; d++)
        fix_bool(&s->drum_mute[d]);
}

void seq_state_write(const seq_t *s, uint8_t *out)
{
    memset(out, 0, SEQ_STATE_BYTES);
    cur_t c = {out, NULL, 0};
    state_io((seq_t *)s, &c); /* the write pass only reads s */
}

bool seq_state_read(seq_t *s, const uint8_t *in)
{
    if(in[0] != SEQ_STATE_VERSION)
        return false;
    halt(s); /* a load is not a player's stop */
    cur_t c = {NULL, in, 0};
    state_io(s, &c);
    state_sanitize(s);
    /* runtime back to rest */
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        s->v[v].gate = s->v[v].tied = s->v[v].dip = false;
        s->v[v].glide_rate = 0.f;
        s->v[v].cur_step   = -1;
    }
    for(int d = 0; d < SEQ_DRUMS; d++)
    {
        s->trig_on[d]   = false;
        s->drum_step[d] = -1;
    }
    s->ext = false;
    s->edges = 0;
    s->period_us = 0;
    s->reset_pending = false;
    s->pos = -START_EPS;
    return true;
}

size_t seq_state_used(void)
{
    static seq_t   tmp;
    static uint8_t scratch[SEQ_STATE_BYTES];
    cur_t c = {scratch, NULL, 0};
    return state_io(&tmp, &c);
}
