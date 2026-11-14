/*
 * FOLLOW: chords from the sung note. See belt_follow.h.
 */
#include "belt_follow.h"

#include <stdlib.h>
#include <string.h>

/* Belt's scale tables (belt_core.c scale_deg order). Chords need seven
 * notes, so the five- and six-note scales borrow their parent: the major
 * pentatonic harmonises in major, minor pentatonic and blues in natural
 * minor, and Chromatic in the major key -- a note outside that set gets a
 * chromatic major or minor chord instead (below). */
static const int kHarm[9][7] = {
    { 0, 2, 4, 5, 7, 9, 11 },   /* Chromatic -> Major */
    { 0, 2, 4, 5, 7, 9, 11 },   /* Major      */
    { 0, 2, 3, 5, 7, 8, 10 },   /* Minor      */
    { 0, 2, 3, 5, 7, 8, 11 },   /* Harm Minor */
    { 0, 2, 3, 5, 7, 9, 10 },   /* Dorian     */
    { 0, 2, 4, 5, 7, 9, 10 },   /* Mixolydian */
    { 0, 2, 4, 5, 7, 9, 11 },   /* Maj Pent -> Major */
    { 0, 2, 3, 5, 7, 8, 10 },   /* Min Pent -> Minor */
    { 0, 2, 3, 5, 7, 8, 10 },   /* Blues    -> Minor */
};

/* How strongly each scale degree's chord is wanted: I, then IV and V,
 * then vi and ii; iii rarely, vii (diminished in major) almost never. */
static const float kDegreeW[7] = { 2.0f, 0.8f, 0.2f, 1.4f, 1.4f, 1.0f, -0.6f };

#define MAX_CHORDS 24
#define WIN        26        /* farthest a voice is placed from the sung note */

typedef struct {
    follow_chord_t c;
    int sixth, eleventh;     /* for CREATIVE's colour roles */
} cand_t;

static int pc_of(int n) { return ((n % 12) + 12) % 12; }

void follow_reset(follow_state_t *f) {
    memset(f, 0, sizeof(*f));
    f->chord.root_pc = -1;
    f->melody = -1;
    for (int i = 0; i < FOLLOW_VOICES; i++) f->notes[i] = -1;
    f->mode = -1;
}

static int build_candidates(cand_t *out, int key, int scale, int mel_pc,
                            int *in_scale) {
    if (scale < 0 || scale > 8) scale = 1;
    const int *d = kHarm[scale];
    int rel = pc_of(mel_pc - key), found = 0;
    for (int i = 0; i < 7; i++) if (d[i] == rel) found = 1;
    *in_scale = found;
    int n = 0;
    if (found) {
        for (int i = 0; i < 7; i++) {
            cand_t *c = &out[n++];
            int r = d[i];
            c->c.root_pc  = pc_of(key + r);
            c->c.degree   = i;
            c->c.third    = pc_of(d[(i + 2) % 7] - r);
            c->c.fifth    = pc_of(d[(i + 4) % 7] - r);
            c->c.seventh  = pc_of(d[(i + 6) % 7] - r);
            c->c.ninth    = pc_of(d[(i + 1) % 7] - r) + 12;
            c->eleventh   = pc_of(d[(i + 3) % 7] - r);
            c->sixth      = pc_of(d[(i + 5) % 7] - r);
        }
        return n;
    }
    /* a note outside the scale: the major and minor chords on every root */
    for (int r = 0; r < 12; r++) {
        for (int minor = 0; minor < 2; minor++) {
            cand_t *c = &out[n++];
            c->c.root_pc = r;
            c->c.degree  = -1;
            c->c.third   = minor ? 3 : 4;
            c->c.fifth   = 7;
            c->c.seventh = 10;
            c->c.ninth   = 14;
            c->eleventh  = minor ? 5 : 6;
            c->sixth     = 9;
        }
    }
    return n;
}

/* the chord tone a pitch class is, given the size: 0 root, 1 third,
 * 2 fifth, 3 seventh, 4 ninth; -1 = not in the chord */
static int role_of(const follow_chord_t *c, int pc, int size) {
    int iv = pc_of(pc - c->root_pc);
    if (iv == 0) return 0;
    if (iv == c->third) return 1;
    if (iv == c->fifth) return 2;
    if (size >= FOLLOW_7TH && iv == c->seventh) return 3;
    if (size >= FOLLOW_9TH && iv == pc_of(c->ninth)) return 4;
    return -1;
}

static float root_motion(int prev_root, int root) {
    if (prev_root < 0) return 0.0f;
    switch (pc_of(root - prev_root)) {
    case 5: case 7:          return 0.8f;
    case 3: case 4: case 8: case 9: return 0.4f;
    case 2: case 10:         return 0.3f;
    case 0:                  return 0.0f;
    default:                 return -0.5f;
    }
}

static float quality_pen(const follow_chord_t *c) {
    return (c->fifth == 6 || c->fifth == 8) ? -1.5f : 0.0f;   /* dim / aug */
}

/* ---- chord choice ------------------------------------------------- */

/* A chord from outside the scale: secondary dominants (a major chord a
 * fifth above a scale note, V of something) first, then chords whose root
 * is at least in the scale. */
static float chromatic_w(const follow_chord_t *c, int key, int scale) {
    const int *d = kHarm[scale < 0 || scale > 8 ? 1 : scale];
    int root_in = 0, resolves = 0;
    for (int i = 0; i < 7; i++) {
        if (pc_of(key + d[i]) == c->root_pc) root_in = 1;
        if (pc_of(key + d[i]) == pc_of(c->root_pc + 5)) resolves = 1;
    }
    float w = -1.0f + (root_in ? 0.5f : 0.0f);
    if (c->third == 4 && resolves) w += 1.0f;
    return w;
}

static int choose_satb(const cand_t *cs, int n, int mel_pc, int size,
                       int sung_bass, int key, int scale,
                       const follow_chord_t *prev) {
    static const float kRoleW[5]     = { 0.6f, 0.4f, 0.2f, 0.0f, -0.2f };
    /* singing the bass line: the note is the root far more often */
    static const float kBassRoleW[5] = { 1.5f, 0.3f, -0.5f, -1.5f, -1.5f };
    int best = -1;
    float bs = -1e9f;
    for (int i = 0; i < n; i++) {
        const follow_chord_t *c = &cs[i].c;
        int role = role_of(c, mel_pc, size);
        if (role < 0) continue;
        float s = (sung_bass ? kBassRoleW : kRoleW)[role] + quality_pen(c);
        s += c->degree >= 0 ? kDegreeW[c->degree] : chromatic_w(c, key, scale);
        if (prev->root_pc >= 0 && prev->root_pc == c->root_pc &&
            prev->third == c->third)
            s += 2.5f;                                   /* stay put */
        s += root_motion(prev->root_pc, c->root_pc);
        if (s > bs) { bs = s; best = i; }
    }
    return best;
}

static int choose_creative(const cand_t *cs, int n, int mel_pc, int color,
                           int key, int scale, const follow_chord_t *prev) {
    const float col = (float)color / 100.0f;
    int best = -1;
    float bs = -1e9f;
    for (int i = 0; i < n; i++) {
        const follow_chord_t *c = &cs[i].c;
        const int iv = pc_of(mel_pc - c->root_pc);
        float s;
        if (iv == pc_of(c->ninth) && c->ninth != 13)   s = 1.5f;      /* 9th */
        else if (iv == c->seventh)                      s = c->seventh == 11 ? 1.5f : 1.2f;
        else if (iv == 9 && cs[i].sixth == 9 && c->fifth == 7) s = 0.5f + 1.5f * col;  /* 6th / 13th */
        else if (iv == cs[i].eleventh && c->third == 3) s = 0.4f + 1.6f * col;  /* m11 */
        else if (iv == 6 && c->third == 4)              s = 2.0f * col - 0.3f;  /* #11 */
        else if (iv == c->third)                        s = 0.4f - 0.8f * col;
        else if (iv == c->fifth)                        s = 0.2f - 1.0f * col;
        else if (iv == 0)                               s = -1.0f - col;
        else continue;                                  /* b9, natural 11 on major ... */
        s += 2.0f * quality_pen(c);
        float fw = c->degree >= 0 ? kDegreeW[c->degree] : chromatic_w(c, key, scale);
        s += fw * (1.0f - 0.6f * col);
        if (c->degree == 1 || c->degree == 2 || c->degree == 5) s += 0.6f * col;
        if (prev->root_pc >= 0 && prev->root_pc == c->root_pc && s > 0.0f)
            s += 1.5f;
        s += root_motion(prev->root_pc, c->root_pc);
        if (s > bs) { bs = s; best = i; }
    }
    return best;
}

/* ---- voicing search ----------------------------------------------- */

typedef struct {
    const follow_chord_t *c;
    int size, creative, color;
    int P, mi, m;
    int has_bass;            /* index 0 is a bass part (root wanted there) */
    int tonemask;            /* pitch classes the voices may take */
    const int *prev;         /* previous voicing (same shape) or NULL */
    int cur[FOLLOW_PARTS];
    int best[FOLLOW_PARTS];
    float best_s;
    int evals;
    int relax;               /* second try: closer spacing, deeper bass */
} search_t;

/* no harmony voice below A1 (55 Hz); below E2 costs */
#define LOW_HARD 33
#define LOW_SOFT 40

static int min_gap(const search_t *s, int i) {   /* between i-1 and i */
    if (s->relax) return 1;
    if (s->creative) return (i == 1 && s->has_bass) ? 5 : 3;
    return 1;
}
static int max_gap(const search_t *s, int i) {
    if (i == 1 && s->has_bass) return s->creative ? 24 : 19;
    return 12;
}

static float score(search_t *s) {
    const int *n = s->cur;
    const follow_chord_t *c = s->c;
    float sc = 0.0f;
    int count[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < s->P; i++) {
        int r = role_of(c, pc_of(n[i]), FOLLOW_9TH);
        if (r == 4 && s->size < FOLLOW_9TH && !s->creative) r = -1;
        if (r == 3 && s->size < FOLLOW_7TH && !s->creative) r = -1;
        if (r >= 0) count[r]++;
        if (i != s->mi) {
            int far = abs(n[i] - s->m) - 19;
            if (far > 0) sc += 0.6f * (float)far;     /* big shifts sound thin */
            if (n[i] < LOW_SOFT)                       /* below E2 gets muddy */
                sc += 0.8f * (float)(LOW_SOFT - n[i]);
        }
    }
    if (!count[1]) sc += 8.0f;                        /* the 3rd says major/minor */
    if (s->has_bass && pc_of(n[0]) != c->root_pc)
        sc += s->creative ? 6.0f : (role_of(c, pc_of(n[0]), s->size) == 1 ? 4.5f : 6.0f);
    if (!s->creative) {
        if (!count[0]) sc += 6.0f;
        if (s->size == FOLLOW_TRIAD) {
            if (!count[2]) sc += 0.7f;
            if (count[1] > 1) sc += 4.0f;             /* doubled 3rd */
            if (count[2] > 1) sc += 0.8f;
        } else {
            if (!count[3]) sc += 5.0f;
            if (s->size == FOLLOW_9TH && !count[4]) sc += 4.0f;
            if (s->size == FOLLOW_7TH && !count[2]) sc += 0.7f;
            for (int r = 1; r < 5; r++) if (count[r] > 1) sc += 2.0f;
        }
    } else {
        const float col = (float)s->color / 100.0f;
        if (!count[3]) sc += 4.0f;                    /* 3rd and 7th: the shell */
        if (!count[4]) sc += 1.5f * col;
        /* the sung colour tone is the top; doubling it inside muddies it */
        for (int i = 0; i < s->P; i++)
            if (i != s->mi && pc_of(n[i]) == pc_of(s->m)) sc += 3.0f;
        for (int i = 2; i < s->P; i++) {
            int g = n[i] - n[i - 1];
            sc += 0.5f * (float)abs(g - 6);           /* open, not clustered */
        }
        for (int i = 1; i < s->P; i++)               /* root lives in the bass */
            if (s->has_bass && i != s->mi && pc_of(n[i]) == c->root_pc) sc += 2.0f;
    }
    if (s->prev) {
        for (int i = 0; i < s->P; i++)
            if (i != s->mi)                           /* basses may leap */
                sc += (i == 0 && s->has_bass ? 0.25f : 0.5f) * (float)abs(n[i] - s->prev[i]);
        /* parallel 5ths and octaves */
        for (int i = 0; i < s->P; i++)
            for (int j = i + 1; j < s->P; j++) {
                int di = n[i] - s->prev[i], dj = n[j] - s->prev[j];
                if (di == 0 || (di > 0) != (dj > 0)) continue;
                int a = pc_of(s->prev[j] - s->prev[i]), b = pc_of(n[j] - n[i]);
                if (a == b && (a == 0 || a == 7)) sc += 5.0f;
            }
    } else {
        /* first chord of a phrase: a close voicing around the sung note */
        for (int i = 0; i < s->P; i++) {
            if (i == s->mi) continue;
            int want = s->m + (i - s->mi) * (s->creative ? 6 : 5);
            if (i == 0 && s->has_bass) want -= s->creative ? 6 : 4;
            sc += 0.25f * (float)abs(n[i] - want);
        }
    }
    return sc;
}

static void search(search_t *s, int i) {
    if (i == s->P) {
        s->evals++;
        float sc = score(s);
        if (sc < s->best_s) {
            s->best_s = sc;
            memcpy(s->best, s->cur, sizeof(s->best));
        }
        return;
    }
    int lo, hi;
    const int win = s->relax ? WIN + 10 : WIN;
    if (i == 0) { lo = s->m - win; hi = s->m + WIN; }
    else {
        lo = s->cur[i - 1] + min_gap(s, i);
        hi = s->cur[i - 1] + max_gap(s, i);
    }
    if (i == s->mi) {
        if (s->m < lo || s->m > hi) return;
        s->cur[i] = s->m;
        search(s, i + 1);
        return;
    }
    if (i < s->mi && hi > s->m - (s->mi - i)) hi = s->m - (s->mi - i);
    if (i > s->mi && hi > s->m + WIN) hi = s->m + WIN;
    if (lo < s->m - win) lo = s->m - win;
    if (lo < LOW_HARD) lo = LOW_HARD;
    for (int n = lo; n <= hi; n++) {
        int pc = pc_of(n);
        if (!(s->tonemask >> pc & 1)) continue;
        if (i == 0 && s->has_bass && s->creative && pc != s->c->root_pc) continue;
        s->cur[i] = n;
        search(s, i + 1);
    }
}

static int tone_mask(const follow_chord_t *c, int size, int creative) {
    int m = 1 << c->root_pc | 1 << pc_of(c->root_pc + c->third);
    if (creative) {
        m |= 1 << pc_of(c->root_pc + c->seventh);
        m |= 1 << pc_of(c->root_pc + c->ninth);
        m |= 1 << pc_of(c->root_pc + c->fifth);
        return m;
    }
    if (size != FOLLOW_9TH) m |= 1 << pc_of(c->root_pc + c->fifth);
    if (size >= FOLLOW_7TH) m |= 1 << pc_of(c->root_pc + c->seventh);
    if (size == FOLLOW_9TH) m |= 1 << pc_of(c->root_pc + c->ninth);
    return m;
}

/* the voices get the other parts, highest first (Belt pans voice 1 hard
 * left, 2 hard right, 3 and 4 nearer the middle) */
static void parts_to_notes(follow_state_t *f) {
    int k = 0;
    for (int i = 0; i < FOLLOW_VOICES; i++) f->notes[i] = -1;
    for (int i = f->n_parts - 1; i >= 0 && k < FOLLOW_VOICES; i--)
        if (i != f->mel_idx) f->notes[k++] = f->parts[i];
}

void follow_commit(follow_state_t *f, int melody, int key, int scale,
                   int mode, int size, int bass, int color) {
    if (mode <= FOLLOW_OFF || mode >= FOLLOW_MODES) { follow_reset(f); return; }
    if (size < FOLLOW_TRIAD || size > FOLLOW_9TH) size = FOLLOW_TRIAD;
    cand_t cs[MAX_CHORDS];
    int in_scale = 0;
    const int mel_pc = pc_of(melody);
    const int nc = build_candidates(cs, key, scale, mel_pc, &in_scale);

    if (mode == FOLLOW_ROOT) {
        /* the sung note is the root: the scale's chord on it, or a major
         * chord when the note is outside the scale */
        const follow_chord_t *c = NULL;
        for (int i = 0; i < nc; i++)
            if (cs[i].c.root_pc == mel_pc && cs[i].c.third == (in_scale ? cs[i].c.third : 4)) {
                c = &cs[i].c;
                break;
            }
        if (!c) return;
        f->chord = *c;
        int p[FOLLOW_PARTS], np = 0;
        if (bass) p[np++] = melody - 12;
        if (size == FOLLOW_TRIAD) p[np++] = melody - 12 + c->fifth;
        p[np++] = melody;
        f->mel_idx = np - 1;
        p[np++] = melody + c->third;
        p[np++] = melody + c->fifth;
        if (size == FOLLOW_7TH) p[np++] = melody + c->seventh;
        if (size == FOLLOW_9TH) p[np++] = melody + c->ninth;
        memcpy(f->parts, p, sizeof(int) * (size_t)np);
        f->n_parts = np;
        f->melody = melody;
        f->mode = mode;
        parts_to_notes(f);
        return;
    }

    const int creative = mode == FOLLOW_CREATIVE;
    int ci = creative ? choose_creative(cs, nc, mel_pc, color, key, scale, &f->chord)
                      : choose_satb(cs, nc, mel_pc, size, mode == FOLLOW_BASS,
                                    key, scale, &f->chord);
    if (ci < 0 && creative)   /* nothing makes it a colour: fall back */
        ci = choose_satb(cs, nc, mel_pc, FOLLOW_9TH, 0, key, scale, &f->chord);
    if (ci < 0) return;

    search_t s;
    memset(&s, 0, sizeof(s));
    s.c = &cs[ci].c;
    s.size = size;
    s.creative = creative;
    s.color = color;
    s.m = melody;
    if (creative) {
        s.has_bass = bass != 0;
        s.P = bass ? 5 : 4;
        s.mi = s.P - 1;
    } else {
        int part = FOLLOW_BASS - mode;             /* 0 bass .. 3 soprano */
        s.has_bass = bass || part == 0;
        s.P = s.has_bass ? 4 : 3;
        s.mi = s.has_bass ? part : part - 1;
    }
    s.tonemask = tone_mask(s.c, size, creative);
    s.prev = (f->mode == mode && f->n_parts == s.P && f->mel_idx == s.mi &&
              f->melody >= 0) ? f->parts : NULL;
    s.best_s = 1e9f;
    search(&s, 0);
    if (s.best_s >= 1e9f) { s.relax = 1; search(&s, 0); }
    if (s.best_s >= 1e9f) return;     /* no voicing: keep the last chord */

    f->chord = *s.c;
    memcpy(f->parts, s.best, sizeof(f->parts));
    f->n_parts = s.P;
    f->mel_idx = s.mi;
    f->melody = melody;
    f->mode = mode;
    parts_to_notes(f);
}
