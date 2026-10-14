/*
 * test_lab_link -- src/lab_link.c, both ends of the two-Lab link, against
 * each other on the laptop. Lab A (USB host, owns the controllers) and Lab B
 * (USB-MIDI device behind A's hub) are wired byte for byte, through the
 * USB-MIDI packet conversion each side's transport does:
 *
 *   - Mini cell <-> message mapping round trips; real Mini input lands on
 *     the cell launchpad.cpp would use;
 *   - USB packets <-> bytes for channel messages and every SysEx length;
 *   - the parser: running status, realtime interleaved, broken SysEx;
 *   - nothing is relayed before Lab B answers the hello (any Daisy-based
 *     MIDI device shares 0483:5740, so the id alone proves nothing);
 *   - the focus combo (top-left + top-right): the first press goes to the
 *     current owner, the completing press and both releases go nowhere;
 *   - with Lab B in focus: Mini, XL and pad input reach B and only B; B's
 *     LEDs are what the controllers show, the logo says whose they are;
 *   - focus back to A: B is told to let go of everything; A's LEDs return;
 *   - unplugging Lab B returns focus and LEDs to A.
 */
#include <stdio.h>
#include <string.h>

#include "lab_link.h"

static int g_fail = 0, g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

/* ---------------------------------------------------------------- mapping */

static void test_mapping(void)
{
    for (int cell = 0; cell < LLK_MINI_CELLS; cell++)
    {
        uint8_t st, d1;
        llk_mini_msg(cell, &st, &d1);
        CHECK(llk_mini_cell(st, d1) == cell);
    }
    /* What the Mini sends in Programmer mode (launchpad.h): note 11 = bottom
     * left pad, CC 19 = bottom of the right column, CC 91 = top row left. */
    CHECK(llk_mini_cell(0x90, 11) == 7 * 8 + 0);
    CHECK(llk_mini_cell(0x80, 88) == 0 * 8 + 7);
    CHECK(llk_mini_cell(0xB0, 19) == 64 + 7);
    CHECK(llk_mini_cell(0xB0, 89) == 64 + 0);
    CHECK(llk_mini_cell(0xB0, 91) == 72);
    CHECK(llk_mini_cell(0xB0, 98) == 79);
    CHECK(llk_mini_cell(0xB0, 99) == LLK_LOGO);
    CHECK(llk_mini_cell(0x98, 41) == -1); /* channel 9 is the XL */
    CHECK(llk_mini_cell(0x90, 10) == -1);
}

/* ------------------------------------------------------------ USB packets */

static void test_usb(void)
{
    uint8_t bytes[64], pk[128], back[64];
    int     n = 0;
    n += llk_enc_mini_led(bytes + n, 12, 21);
    n += llk_enc_hello(bytes + n);           /* 5-byte SysEx */
    n += llk_enc_presence(bytes + n, 3);     /* 6 */
    n += llk_enc_pad(bytes + n, 0x2ABCD);    /* 8 */
    n += llk_enc_xl_led(bytes + n, 33, 0x3C); /* 11 */
    bytes[n++] = 0xB8; bytes[n++] = 13; bytes[n++] = 64;
    int       used = 0;
    const int len  = llk_to_usb(bytes, n, pk, sizeof pk, &used);
    CHECK(used == n);
    CHECK(len % 4 == 0);
    CHECK(pk[0] == 0x09 && pk[1] == 0x90);
    const int m = llk_from_usb(pk, len, back, sizeof back);
    CHECK(m == n && memcmp(back, bytes, (size_t)n) == 0);
    /* A buffer too small for the next whole message stops before it. */
    const int part = llk_to_usb(bytes, n, pk, 8, &used);
    CHECK(part == 4 && used == 3);
}

/* ----------------------------------------------------------------- parser */

static uint8_t g_seen[16][24];
static int     g_seen_n[16], g_nseen;

static void collect(const uint8_t* m, int n, void* ctx)
{
    (void)ctx;
    if (g_nseen < 16) { memcpy(g_seen[g_nseen], m, (size_t)n); g_seen_n[g_nseen++] = n; }
}

static void test_parser(void)
{
    llk_parser_t p;
    memset(&p, 0, sizeof p);
    g_nseen = 0;
    const uint8_t s[] = {0x90, 60, 100, 62, 100,          /* running status */
                         0xF8,                            /* clock in the middle */
                         0xF0, 0x7D, 0x4C, 0x01, 0xF7,    /* SysEx */
                         0xF0, 0x01, 0x02, 0xB0, 7, 64,   /* broken SysEx, then a CC */
                         0x40};                           /* stray data */
    llk_parse(&p, s, sizeof s, collect, NULL);
    CHECK(g_nseen == 4);
    CHECK(g_seen_n[0] == 3 && g_seen[0][1] == 60);
    CHECK(g_seen_n[1] == 3 && g_seen[1][0] == 0x90 && g_seen[1][1] == 62);
    CHECK(g_seen_n[2] == 5 && g_seen[2][3] == 0x01);
    CHECK(g_seen_n[3] == 3 && g_seen[3][0] == 0xB0 && g_seen[3][2] == 64);
    /* split across feeds */
    g_nseen = 0;
    llk_parse(&p, (const uint8_t[]){0xF0, 0x7D}, 2, collect, NULL);
    llk_parse(&p, (const uint8_t[]){0x4C, 0x02, 0xF7}, 3, collect, NULL);
    CHECK(g_nseen == 1 && g_seen[0][3] == 0x02);
}

/* -------------------------------------------------------------- two Labs */

typedef struct
{
    llk_client_t c;
    uint8_t      mini[64][3];
    int          nmini;
    uint8_t      xl[64][3];
    int          nxl;
    int          hellos, releases;
    uint8_t      presence;
    uint32_t     pad;
    uint8_t      to_a[512]; /* what B sends back */
    int          to_a_n;
} LabB;

static void b_mini(uint8_t st, uint8_t d1, uint8_t d2, void* ctx)
{
    LabB* b = (LabB*)ctx;
    if (b->nmini < 64) { b->mini[b->nmini][0] = st; b->mini[b->nmini][1] = d1; b->mini[b->nmini][2] = d2; b->nmini++; }
}
static void b_xl(uint8_t st, uint8_t d1, uint8_t d2, void* ctx)
{
    LabB* b = (LabB*)ctx;
    if (b->nxl < 64) { b->xl[b->nxl][0] = st; b->xl[b->nxl][1] = d1; b->xl[b->nxl][2] = d2; b->nxl++; }
}
static void b_hello(void* ctx)
{
    LabB* b = (LabB*)ctx;
    b->hellos++;
    b->to_a_n += llk_enc_hello_reply(b->to_a + b->to_a_n);
}
static void b_release(void* ctx) { ((LabB*)ctx)->releases++; }
static void b_presence(uint8_t bits, void* ctx) { ((LabB*)ctx)->presence = bits; }
static void b_pad(uint32_t m, void* ctx) { ((LabB*)ctx)->pad = m; }

static void labb_init(LabB* b)
{
    memset(b, 0, sizeof *b);
    const llk_client_cb_t cb = {b_mini, b_xl, b_hello, b_release, b_presence, b_pad, b};
    llk_client_init(&b->c, &cb);
}

/* A -> B through USB packets, as the hub driver and libDaisy carry them. */
static void a_to_b(LabB* b, const uint8_t* bytes, int n)
{
    uint8_t pk[512], back[512];
    int     used = 0;
    const int len = llk_to_usb(bytes, n, pk, sizeof pk, &used);
    CHECK(used == n);
    const int m = llk_from_usb(pk, len, back, sizeof back);
    llk_client_feed(&b->c, back, m);
}
static void b_to_a(llk_host_t* h, LabB* b)
{
    uint8_t pk[1024], back[1024];
    int     used = 0;
    const int len = llk_to_usb(b->to_a, b->to_a_n, pk, sizeof pk, &used);
    const int m   = llk_from_usb(pk, len, back, sizeof back);
    llk_host_from_b(h, back, m);
    b->to_a_n = 0;
}

static void test_two_labs(void)
{
    llk_host_t a;
    LabB       b;
    uint8_t    out[256];
    int        n = 0;
    llk_host_init(&a);
    labb_init(&b);

    /* Not a Lab yet: everything stays local, nothing is relayed. */
    llk_host_attach(&a);
    CHECK(a.state == LLK_CANDIDATE);
    CHECK(llk_host_mini_in(&a, 0xB0, 91, 127, out, &n) == LLK_LOCAL);
    CHECK(llk_host_mini_in(&a, 0xB0, 98, 127, out, &n) == LLK_LOCAL); /* no combo before linking */
    llk_host_mini_in(&a, 0xB0, 91, 0, out, &n);
    llk_host_mini_in(&a, 0xB0, 98, 0, out, &n);
    CHECK(n == 0 && a.focus_b == 0);
    llk_host_presence(&a, LLK_HAS_MINI, out, &n);
    CHECK(n == 0);
    CHECK(llk_host_mini_led(&a, LLK_LOGO, 9) == 9);

    /* Hello -> reply -> linked. */
    n = llk_enc_hello(out);
    a_to_b(&b, out, n);
    CHECK(b.hellos == 1);
    b_to_a(&a, &b);
    CHECK(a.state == LLK_LINKED && a.replies == 1);
    n = 0;
    llk_host_presence(&a, LLK_HAS_MINI | LLK_HAS_XL, out, &n);
    llk_host_presence(&a, LLK_HAS_MINI | LLK_HAS_XL, out, &n); /* unchanged: sent once */
    a_to_b(&b, out, n);
    CHECK(b.presence == (LLK_HAS_MINI | LLK_HAS_XL));

    /* B draws its LEDs while A still has focus: kept, not shown. */
    b.to_a_n += llk_enc_mini_led(b.to_a + b.to_a_n, 10, 45);
    b.to_a_n += llk_enc_mini_led(b.to_a + b.to_a_n, 72, 5);
    b.to_a_n += llk_enc_xl_led(b.to_a + b.to_a_n, 30, 0x3C);
    b_to_a(&a, &b);
    CHECK(a.b_mini[10] == 45 && a.b_mini[72] == 5 && a.b_xl[30] == 0x3C);
    CHECK(llk_host_mini_led(&a, 10, 7) == 7);
    CHECK(llk_host_xl_led(&a, 30, 0x0C) == 0x0C);
    CHECK(llk_host_mini_led(&a, LLK_LOGO, 0) == LLK_LOGO_A);

    /* A's own input while A has focus. */
    n = 0;
    CHECK(llk_host_mini_in(&a, 0x90, 55, 127, out, &n) == LLK_LOCAL);
    CHECK(llk_host_xl_in(&a, 0xB8, 13, 64, out, &n) == LLK_LOCAL);
    llk_host_pad(&a, 0x1000, out, &n);
    CHECK(n == 0);

    /* The combo: first press to the owner (A), second switches, releases vanish. */
    CHECK(llk_host_mini_in(&a, 0xB0, 91, 127, out, &n) == LLK_LOCAL);
    CHECK(llk_host_mini_in(&a, 0xB0, 98, 127, out, &n) == LLK_TOGGLED);
    CHECK(a.focus_b == 1 && llk_host_b_owns(&a));
    CHECK(n == 0); /* A -> B needs no message (B had nothing held) */
    CHECK(llk_host_mini_in(&a, 0xB0, 98, 0, out, &n) == LLK_SWALLOWED);
    CHECK(llk_host_mini_in(&a, 0xB0, 91, 0, out, &n) == LLK_SWALLOWED);
    CHECK(n == 0);

    /* B in focus: input reaches B, B's LEDs are shown. */
    CHECK(llk_host_mini_in(&a, 0x90, 55, 127, out, &n) == LLK_FORWARDED);
    CHECK(llk_host_mini_in(&a, 0x90, 55, 0, out, &n) == LLK_FORWARDED);
    CHECK(llk_host_mini_in(&a, 0xB0, 95, 127, out, &n) == LLK_FORWARDED); /* other top buttons pass */
    CHECK(llk_host_xl_in(&a, 0xB8, 13, 64, out, &n) == LLK_FORWARDED);
    CHECK(llk_host_xl_in(&a, 0x98, 41, 127, out, &n) == LLK_FORWARDED);
    llk_host_pad(&a, (1u << 12) | (1u << 16), out, &n);
    llk_host_pad(&a, (1u << 12) | (1u << 16), out, &n); /* unchanged: once */
    a_to_b(&b, out, n);
    CHECK(b.nmini == 3 && b.mini[0][0] == 0x90 && b.mini[0][1] == 55 && b.mini[0][2] == 127);
    CHECK(b.mini[2][0] == 0xB0 && b.mini[2][1] == 95);
    CHECK(b.nxl == 2 && b.xl[0][0] == 0xB8 && b.xl[0][1] == 13 && b.xl[0][2] == 64);
    CHECK(b.xl[1][0] == 0x98 && b.xl[1][1] == 41);
    CHECK(b.pad == ((1u << 12) | (1u << 16)));
    CHECK(llk_host_mini_led(&a, 10, 7) == 45);
    CHECK(llk_host_mini_led(&a, 72, 7) == 5);
    CHECK(llk_host_xl_led(&a, 30, 0x0C) == 0x3C);
    CHECK(llk_host_mini_led(&a, LLK_LOGO, 0) == LLK_LOGO_B);

    /* Back to A: B is told to let go. */
    n = 0;
    CHECK(llk_host_mini_in(&a, 0xB0, 98, 127, out, &n) == LLK_FORWARDED); /* first press: B's */
    CHECK(llk_host_mini_in(&a, 0xB0, 91, 127, out, &n) == LLK_TOGGLED);
    a_to_b(&b, out, n);
    CHECK(a.focus_b == 0);
    CHECK(b.releases == 1);
    CHECK(b.pad == 0);
    n = 0;
    CHECK(llk_host_mini_in(&a, 0xB0, 91, 0, out, &n) == LLK_SWALLOWED);
    CHECK(llk_host_mini_in(&a, 0xB0, 98, 0, out, &n) == LLK_SWALLOWED);
    CHECK(llk_host_mini_in(&a, 0x90, 55, 127, out, &n) == LLK_LOCAL);
    CHECK(llk_host_mini_led(&a, 10, 7) == 7);
    CHECK(n == 0);

    /* Explicit focus (a Settings toggle), then unplug: A gets everything back. */
    llk_host_set_focus(&a, 1, out, &n);
    CHECK(a.focus_b == 1);
    llk_host_detach(&a);
    CHECK(a.state == LLK_NONE && a.focus_b == 0);
    CHECK(llk_host_mini_led(&a, 10, 7) == 7 && llk_host_mini_led(&a, LLK_LOGO, 9) == 9);
    CHECK(llk_host_mini_in(&a, 0x90, 55, 127, out, &n) == LLK_LOCAL);
    /* Focus cannot go to a Lab that is not linked. */
    llk_host_attach(&a);
    llk_host_set_focus(&a, 1, out, &n);
    CHECK(a.focus_b == 0);
}

int main(void)
{
    test_mapping();
    test_usb();
    test_parser();
    test_two_labs();
    printf("test_lab_link: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
