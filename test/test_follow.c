/*
 * FOLLOW, natively, at 48 kHz, through the firmware's own engine unit,
 * with a synthesized singer: a band-limited buzz through three vowel
 * formants, 5.5 Hz vibrato, a scoop up into each note, breaths between
 * phrases and short consonant gaps between notes.
 *
 *  1. ROOT: each sung note is the chord root, 3rd and 5th above, root an
 *     octave below; the voices really sound at those pitches.
 *  2. SOPRANO / ALTO / TENOR / BASS / CREATIVE over a little tune in C major:
 *     the sung note is always in the chord, every voice is a real note, the
 *     parts move by small steps.
 *  3. STICK: a 60 ms passing note does not change the chord at the default
 *     STICK, and does at STICK fully left.
 *  4. A consonant gap or a scoop never flickers the chord.
 *  5. HOLD freezes the chord while the singer moves on.
 *  6. How long a chord change takes, and what FOLLOW costs per block.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/versio_alloc.h"
#include "belt_core.h"

#define BLK 128
#define SR  BELT_SR
#define CAP 16384

static float fake_bpm(void) { return 120.0f; }

static float cap[CAP];
static long  cap_n = 0;
static belt_t *B;

/* chord read-back, refreshed every block */
typedef struct { int root, third, n[4]; } chord_t;
static chord_t cur;
static int     changes;          /* chord changes seen since last reset */
static double  t_now, t_change;  /* seconds */

static chord_t read_chord(void)
{
    char buf[64];
    chord_t c;
    belt_get_param(B, "chord", buf, sizeof buf);
    sscanf(buf, "%d:%d:%d,%d,%d,%d", &c.root, &c.third, &c.n[0], &c.n[1], &c.n[2], &c.n[3]);
    return c;
}

/* ---- the singer ---- */
static double ph, fm_z[3][2];
static double synth(double hz, double amp)
{
    /* buzz */
    int nh = (int)(9000.0 / hz);
    if (nh > 60) nh = 60;
    double x = 0.0;
    for (int k = 1; k <= nh; k++) x += sin(ph * k) / k;
    ph += 2.0 * M_PI * hz / SR;
    if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
    /* "ah": three two-pole resonators in parallel */
    static const double F[3] = {730, 1090, 2440}, BW[3] = {90, 110, 160}, G[3] = {1.0, 0.5, 0.25};
    double y = 0.0;
    for (int f = 0; f < 3; f++)
    {
        double r = exp(-M_PI * BW[f] / SR), c = 2.0 * r * cos(2.0 * M_PI * F[f] / SR);
        double o = (1.0 - r) * x + c * fm_z[f][0] - r * r * fm_z[f][1];
        fm_z[f][1] = fm_z[f][0];
        fm_z[f][0] = o;
        y += G[f] * o;
    }
    return y * amp * 1.6 + x * amp * 0.08;
}

static void blockrun(double hz_of_t(double, void *), void *ctx, double amp_of_t(double, double, void *),
                     double seconds, double start)
{
    int16_t io[BLK * 2];
    long total = (long)(seconds * SR);
    for (long done = 0; done < total; done += BLK)
    {
        for (int i = 0; i < BLK; i++)
        {
            double t = start + (double)(done + i) / SR;
            double hz = hz_of_t(t, ctx), a = amp_of_t(t, start + seconds, ctx);
            double v = hz > 0 ? synth(hz, a) : 0.0;
            if (v > 0.95) v = 0.95;
            if (v < -0.95) v = -0.95;
            io[2 * i] = io[2 * i + 1] = (int16_t)(v * 32767.0);
        }
        belt_process(B, io, io, BLK);
        for (int i = 0; i < BLK; i++)
            cap[(cap_n++) % CAP] = 0.5f * ((float)io[2 * i] + (float)io[2 * i + 1]) / 32768.0f;
        t_now += (double)BLK / SR;
        chord_t c = read_chord();
        if (memcmp(&c, &cur, sizeof c))
        {
            cur = c;
            changes++;
            t_change = t_now;
        }
    }
}

/* one sung note: scoop up 70 cents over 60 ms, vibrato after 150 ms */
typedef struct { int midi; double t0; } note_ctx;
static double note_hz(double t, void *v)
{
    note_ctx *n = v;
    double dt = t - n->t0, cents = 0.0;
    if (dt < 0.06) cents = -70.0 * (1.0 - dt / 0.06);
    double depth = dt < 0.15 ? 0.0 : (dt < 0.4 ? (dt - 0.15) / 0.25 : 1.0) * 30.0;
    cents += depth * sin(2.0 * M_PI * 5.5 * dt);
    return 440.0 * pow(2.0, (n->midi - 69 + cents / 100.0) / 12.0);
}
static double note_amp(double t, double end, void *v)
{
    note_ctx *n = v;
    double a = 0.35, dt = t - n->t0, left = end - t;
    if (dt < 0.02) a *= dt / 0.02;
    if (left < 0.02) a *= left / 0.02;
    return a;
}
static double zero_hz(double t, void *v) { (void)t; (void)v; return 0.0; }
static double zero_amp(double t, double e, void *v) { (void)t; (void)e; (void)v; return 0.0; }

static void sing(int midi, double seconds)
{
    note_ctx n = {midi, t_now};
    blockrun(note_hz, &n, note_amp, seconds, t_now);
}
static void rest(double seconds) { blockrun(zero_hz, NULL, zero_amp, seconds, t_now); }

/* ---- measuring ---- */
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
static double mhz(int m) { return 440.0 * pow(2.0, (m - 69) / 12.0); }

static const char *NAME[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static void nn(int m, char *o) { sprintf(o, "%s%d", NAME[((m % 12) + 12) % 12], m / 12 - 1); }
static void print_chord(const char *tag, int sung)
{
    char s[8], v[4][8];
    nn(sung, s);
    for (int i = 0; i < 4; i++)
    {
        if (cur.n[i] < 0) strcpy(v[i], "-");
        else nn(cur.n[i], v[i]);
    }
    printf("  %-9s sing %-4s -> %s%s  voices %s %s %s %s\n", tag, s, NAME[cur.root],
           cur.third == 3 ? "m" : "", v[0], v[1], v[2], v[3]);
}

static void set(const char *k, int v)
{
    char s[16];
    snprintf(s, sizeof s, "%d", v);
    belt_set_param(B, k, s);
}

static int in_chord(int pc)
{
    /* the sung pitch class is a triad tone of the chord */
    int iv = ((pc - cur.root) % 12 + 12) % 12;
    return iv == 0 || iv == cur.third || (iv >= 6 && iv <= 8);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *pool = malloc(VERSIO_POOL_BYTES);
    assert(pool);
    versio_alloc_init(pool, VERSIO_POOL_BYTES);
    host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version      = 1;
    host.sample_rate      = SR;
    host.frames_per_block = BLK;
    host.get_bpm          = fake_bpm;
    B = belt_create(&host);
    assert(B && !versio_alloc_failed());

    set("monitor", 1);
    set("hw_input", 1);
    set("humanize", 0);
    set("key", 0);    /* C */
    set("scale", 1);  /* Major */
    set("wet", 100);
    set("harm_level", 100);
    cur = read_chord();

    /* ---- 1. ROOT, and the voices really sing those notes ---- */
    printf("ROOT, C major:\n");
    set("follow", 1);
    static const int root_mel[4] = {60, 65, 67, 57};
    for (int k = 0; k < 4; k++)
    {
        sing(root_mel[k], 1.2);
        print_chord("root", root_mel[k]);
        assert(cur.root == root_mel[k] % 12);
        assert(cur.n[0] >= 0 && cur.n[1] >= 0 && cur.n[2] >= 0 && cur.n[3] >= 0);
        /* each voice: well above the level a semitone away */
        for (int i = 0; i < 4; i++)
        {
            double on = power(mhz(cur.n[i])), off = power(mhz(cur.n[i] + 1));
            printf("            voice %d  on %.5f  semitone up %.5f\n", i + 1, on, off);
            assert(on > 4.0 * off);
        }
        rest(0.15);
    }

    /* ---- 2. the other modes over a little tune ---- */
    static const int   tune[]  = {64, 62, 60, 62, 64, 64, 64, 62, 62, 62, 64, 67, 67};
    static const char *mname[] = {"", "Root", "Soprano", "Alto", "Tenor", "Bass", "Creative"};
    static const int   octave[] = {0, 0, 0, -5, -12, -19, 0};
    for (int mode = 2; mode <= 6; mode++)
    {
        printf("%s, C major (stick default):\n", mname[mode]);
        set("follow", mode);
        rest(0.6);
        int prev[4] = {-1, -1, -1, -1};
        int big_leaps = 0;
        for (size_t k = 0; k < sizeof tune / sizeof tune[0]; k++)
        {
            int m = tune[k] + octave[mode];
            if (mode == 5 && m < 40) m += 12;
            sing(m, 0.45);
            rest(0.05);  /* a consonant */
            print_chord(mname[mode], m);
            if (mode != 6) assert(in_chord(m % 12));
            for (int i = 0; i < 4; i++)
            {
                if (cur.n[i] >= 0 && prev[i] >= 0 && abs(cur.n[i] - prev[i]) > 7) big_leaps++;
                prev[i] = cur.n[i];
            }
        }
        printf("            leaps over a 5th in the voices: %d\n", big_leaps);
        if (mode >= 2 && mode <= 5) assert(big_leaps <= 2);
    }

    /* ---- 3. STICK ---- */
    printf("STICK (Soprano):\n");
    set("follow", 2);
    set("follow_stick", 40);
    rest(0.6);
    sing(64, 0.8);
    chord_t before = cur;
    int c0 = changes;
    sing(65, 0.06);  /* F, 60 ms passing note */
    sing(64, 0.8);
    printf("  default:   60 ms passing F -> %d change(s)\n", changes - c0);
    assert(!memcmp(&before, &cur, sizeof cur));
    set("follow_stick", 0);
    sing(64, 0.3);
    c0 = changes;
    sing(65, 0.2);
    printf("  fully left: 200 ms F -> %d change(s)\n", changes - c0);
    assert(changes - c0 >= 1);
    set("follow_stick", 40);

    /* ---- 4. gaps and scoops don't flicker ---- */
    printf("Gaps:\n");
    sing(67, 0.8);
    c0 = changes;
    for (int r = 0; r < 6; r++) { rest(0.07); sing(67, 0.25); }
    printf("  G4 x6 with 70 ms gaps -> %d change(s)\n", changes - c0);
    assert(changes == c0);

    /* ---- 5. HOLD ---- */
    printf("HOLD:\n");
    set("follow", 1);
    sing(60, 1.0);
    chord_t held = cur;
    print_chord("held", 60);
    set("hold", 1);
    sing(62, 0.4);
    sing(62, 1.0);
    double p_held = 0, p_new = 0;
    for (int i = 0; i < 4; i++) p_held += power(mhz(held.n[i]));
    /* D root: F#3? no -- what ROOT on D would sing: D3, D4+4=F#4, A4 */
    p_new = power(mhz(62 - 12)) + power(mhz(62 + 4)) + power(mhz(62 + 7));
    printf("  singing D4 over held C: held chord %.5f  D chord %.5f\n", p_held, p_new);
    assert(p_held > 3.0 * p_new);
    set("hold", 0);

    /* ---- 6. latency and cost ---- */
    printf("Latency (time from the note's start to the new chord):\n");
    for (int sk = 0; sk <= 100; sk += 40)
    {
        set("follow", 2);
        set("follow_stick", sk);
        sing(64, 0.8);
        double t0 = t_now;
        c0 = changes;
        sing(67, 1.0);
        printf("  stick %3d: %s %.0f ms\n", sk, changes > c0 ? "" : "(no change!)", (t_change - t0) * 1000.0);
        assert(changes > c0);
    }
    /* first note of a phrase */
    rest(0.6);
    double t0 = t_now;
    c0 = changes;
    sing(69, 0.5);
    printf("  first note after a breath: %.0f ms (%d change(s))\n", (t_change - t0) * 1000.0, changes - c0);

    set("follow_stick", 40);
    int16_t io[BLK * 2];
    for (int mode = 0; mode <= 6; mode += 6)
    {
        set("follow", mode);
        clock_t c = clock();
        note_ctx n = {64, t_now};
        for (int r = 0; r < 2000; r++)
        {
            for (int i = 0; i < BLK; i++) io[2 * i] = io[2 * i + 1] = (int16_t)(synth(note_hz(t_now + (double)i / SR, &n), 0.3) * 32767.0);
            belt_process(B, io, io, BLK);
        }
        printf("  %s: %.1f us/block (host)\n", mode ? "Creative" : "Off     ",
               1e6 * (double)(clock() - c) / CLOCKS_PER_SEC / 2000.0);
    }
    puts("test_follow ok");
    return 0;
}
