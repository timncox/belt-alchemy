/*
 * test_usb_keys -- the hardware-free half of the Lab's USB host
 * (src/usb_midi_keys.c): configuration descriptors -> what the hub driver
 * opens and whether libDaisy's plain MIDI host could serve the device,
 * vendor/product -> role, and USB-MIDI packets -> the notes Belt is given.
 *
 * Descriptors are built here from their parts. The Launchpad Mini MK3,
 * Launch Control XL and XInput pad ones are reconstructed from what was read
 * off the real devices (interfaces, endpoints, sizes -- memory
 * project_alchemy_family); the cs-jack bytes in between are the spec's, not
 * a byte dump. The keyboards follow USB-MIDI 1.0 Appendix B.
 */
#include <stdio.h>
#include <string.h>

#include "usb_midi_keys.h"

static int g_fail = 0, g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

/* ---------------------------------------------------------------- builder */

typedef struct { uint8_t b[1024]; uint16_t n; } Desc;

static void put(Desc *d, const uint8_t *p, int n) { memcpy(d->b + d->n, p, (size_t)n); d->n = (uint16_t)(d->n + n); }

static void cfg_begin(Desc *d, uint8_t nif)
{
    const uint8_t c[9] = {9, 0x02, 0, 0, nif, 1, 0, 0x80, 50};
    d->n = 0;
    put(d, c, 9);
}
static void cfg_end(Desc *d) { d->b[2] = (uint8_t)(d->n & 0xFF); d->b[3] = (uint8_t)(d->n >> 8); }

static void itf(Desc *d, uint8_t num, uint8_t alt, uint8_t neps, uint8_t cls, uint8_t sub, uint8_t proto)
{
    const uint8_t c[9] = {9, 0x04, num, alt, neps, cls, sub, proto, 0};
    put(d, c, 9);
}
/* Audio-class endpoints are 9 bytes (bRefresh, bSynchAddress). */
static void ep9(Desc *d, uint8_t addr, uint8_t attr, uint16_t mps)
{
    const uint8_t c[9] = {9, 0x05, addr, attr, (uint8_t)mps, (uint8_t)(mps >> 8), 0, 0, 0};
    put(d, c, 9);
}
static void ep7(Desc *d, uint8_t addr, uint8_t attr, uint16_t mps, uint8_t interval)
{
    const uint8_t c[7] = {7, 0x05, addr, attr, (uint8_t)mps, (uint8_t)(mps >> 8), interval};
    put(d, c, 7);
}
static void blob(Desc *d, uint8_t type, int len)
{
    uint8_t c[64] = {0};
    c[0] = (uint8_t)len;
    c[1] = type;
    put(d, c, len);
}
static void ac(Desc *d, uint8_t num) /* Audio Control + its class header */
{
    itf(d, num, 0, 0, 0x01, 0x01, 0x00);
    blob(d, 0x24, 9);
}
/* MS interface body: header, the jacks for n cables, then the endpoints,
 * each with its class-specific endpoint descriptor. */
static void ms(Desc *d, uint8_t num, uint8_t alt, uint8_t cables, uint8_t ep_out, uint8_t ep_in,
               uint16_t mps)
{
    itf(d, num, alt, (uint8_t)((ep_out ? 1 : 0) + (ep_in ? 1 : 0)), 0x01, 0x03, 0x00);
    blob(d, 0x24, 7);
    for (int i = 0; i < cables; i++)
    {
        blob(d, 0x24, 6); blob(d, 0x24, 6); /* IN jacks, embedded + external */
        blob(d, 0x24, 9); blob(d, 0x24, 9); /* OUT jacks */
    }
    if (ep_out) { ep9(d, ep_out, 0x02, mps); blob(d, 0x25, (uint8_t)(4 + cables)); }
    if (ep_in)  { ep9(d, ep_in, 0x02, mps);  blob(d, 0x25, (uint8_t)(4 + cables)); }
}

/* ---------------------------------------------------------------- descriptors */

static int direct(const Desc *d) { return umk_direct_ok(d->b, d->n, 2, 2, 256); }

static void test_descriptors(void)
{
    Desc      d;
    umk_cfg_t f;

    /* A plain keyboard, USB-MIDI 1.0 Appendix B: AC, then MS with bulk OUT
     * 0x01 and IN 0x81. */
    cfg_begin(&d, 2); ac(&d, 0); ms(&d, 1, 0, 1, 0x01, 0x81, 64); cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(f.in_ep == 0x81 && f.out_ep == 0x01 && f.in_size == 64 && f.out_size == 64);
    CHECK(f.iface == 1);
    CHECK(direct(&d) == 1);

    /* Keys only: no OUT endpoint at all. The hub driver takes it now. */
    cfg_begin(&d, 2); ac(&d, 0); ms(&d, 1, 0, 1, 0, 0x82, 64); cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(f.in_ep == 0x82 && f.out_ep == 0 && f.out_size == 0);
    CHECK(direct(&d) == 1);

    /* An OUT-only MIDI device (a sound module): nothing to read, not taken. */
    cfg_begin(&d, 2); ac(&d, 0); ms(&d, 1, 0, 1, 0x01, 0, 64); cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_NONE);
    CHECK(f.in_ep == 0 && f.out_ep == 0);
    CHECK(direct(&d) == 0);

    /* Launchpad Mini MK3: AC, MS with two cables (OUT 0x01, IN 0x81, 64 B),
     * HID, mass storage. */
    cfg_begin(&d, 4); ac(&d, 0); ms(&d, 1, 0, 2, 0x01, 0x81, 64);
    itf(&d, 2, 0, 1, 0x03, 0x00, 0x00); blob(&d, 0x21, 9); ep7(&d, 0x83, 0x03, 64, 1);
    itf(&d, 3, 0, 2, 0x08, 0x06, 0x50); ep7(&d, 0x84, 0x02, 64, 0); ep7(&d, 0x04, 0x02, 64, 0);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(f.in_ep == 0x81 && f.out_ep == 0x01 && f.in_size == 64);
    CHECK(direct(&d) == 1);

    /* Launch Control XL: AC, MS one cable, IN 0x81 / OUT 0x02. */
    cfg_begin(&d, 2); ac(&d, 0); ms(&d, 1, 0, 1, 0x02, 0x81, 64); cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(f.in_ep == 0x81 && f.out_ep == 0x02);

    /* Xbox 360 wired / GP2040-CE XInput: ff/5d/01 (int IN 0x81 32 B every
     * 1 ms, int OUT 0x02), then 5d/03 (4 eps), 5d/02, security fd/13. */
    cfg_begin(&d, 4);
    itf(&d, 0, 0, 2, 0xFF, 0x5D, 0x01); blob(&d, 0x21, 17);
    ep7(&d, 0x81, 0x03, 32, 1); ep7(&d, 0x02, 0x03, 32, 8);
    itf(&d, 1, 0, 4, 0xFF, 0x5D, 0x03); blob(&d, 0x21, 27);
    ep7(&d, 0x83, 0x03, 32, 2); ep7(&d, 0x04, 0x03, 32, 4);
    ep7(&d, 0x85, 0x03, 32, 64); ep7(&d, 0x05, 0x03, 32, 16);
    itf(&d, 2, 0, 1, 0xFF, 0x5D, 0x02); blob(&d, 0x21, 9); ep7(&d, 0x86, 0x03, 32, 16);
    itf(&d, 3, 0, 0, 0xFF, 0xFD, 0x13); blob(&d, 0x41, 6);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_XINPUT);
    CHECK(f.in_ep == 0x81 && f.in_size == 32 && f.interval == 1);
    CHECK(f.out_ep == 0x02 && f.out_size == 32);
    CHECK(direct(&d) == 0);

    /* The Realtek 0bda:8153 Ethernet chip in Tim's dongle: vendor class,
     * bulk + interrupt. Not MIDI: the hub switches its port off. */
    cfg_begin(&d, 1); itf(&d, 0, 0, 3, 0xFF, 0xFF, 0x00);
    ep7(&d, 0x81, 0x02, 64, 0); ep7(&d, 0x02, 0x02, 64, 0); ep7(&d, 0x83, 0x03, 2, 8);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_NONE);
    CHECK(direct(&d) == 0);

    /* HID first (a keyboard with a vendor/HID control interface ahead of
     * MIDI): the hub path finds MIDI; libDaisy's plain host picks its class
     * from interface 0, HID, which is not registered -> needs the hub. */
    cfg_begin(&d, 3);
    itf(&d, 0, 0, 1, 0x03, 0x00, 0x00); blob(&d, 0x21, 9); ep7(&d, 0x83, 0x03, 8, 10);
    ac(&d, 1); ms(&d, 2, 0, 1, 0x01, 0x81, 64);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(f.in_ep == 0x81 && f.iface == 2);
    CHECK(direct(&d) == 0);

    /* A keyboard with an audio interface: AC, audio streaming alt 0 / alt 1
     * (isochronous), then MS -- MIDI is past the two interfaces libDaisy
     * keeps. */
    cfg_begin(&d, 3); ac(&d, 0);
    itf(&d, 1, 0, 0, 0x01, 0x02, 0x00);
    itf(&d, 1, 1, 1, 0x01, 0x02, 0x00); blob(&d, 0x24, 7); blob(&d, 0x24, 11);
    ep9(&d, 0x03, 0x05, 192); blob(&d, 0x25, 7);
    ms(&d, 2, 0, 1, 0x02, 0x82, 64);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(f.in_ep == 0x82 && f.out_ep == 0x02 && f.iface == 2);
    CHECK(direct(&d) == 0);

    /* USB-MIDI 2.0 device: MS alt 0 is MIDI 1.0 (0x01/0x81), alt 1 is UMP
     * (0x02/0x82). Nothing selects alt 1, so alt 0's endpoints are the live
     * ones. */
    cfg_begin(&d, 2); ac(&d, 0);
    ms(&d, 1, 0, 1, 0x01, 0x81, 64);
    ms(&d, 1, 1, 1, 0x02, 0x82, 64);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(f.in_ep == 0x81 && f.out_ep == 0x01);
    CHECK(direct(&d) == 1);

    /* MIDI only on an alternate setting (alt 0 empty): never selected by
     * this host, so not taken. */
    cfg_begin(&d, 2); ac(&d, 0);
    itf(&d, 1, 0, 0, 0x01, 0x03, 0x00); blob(&d, 0x24, 7);
    ms(&d, 1, 1, 1, 0x02, 0x82, 64);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_NONE);

    /* A MIDI interface on interrupt endpoints (not class-compliant; some
     * cheap devices): not taken -- bulk only. */
    cfg_begin(&d, 2); ac(&d, 0);
    itf(&d, 1, 0, 2, 0x01, 0x03, 0x00); blob(&d, 0x24, 7);
    ep9(&d, 0x01, 0x03, 64); ep9(&d, 0x81, 0x03, 64);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_NONE);

    /* A long descriptor (16 cables: 9+9+9+9+7 + 16*30 + 2*(9+20) = 581 B).
     * The hub driver reads up to 512 B: the endpoints are past that, so it
     * is refused, not misread. libDaisy's plain host is clamped to 256 B
     * (launchpad.cpp) and gets the interface but no endpoint -> no. */
    cfg_begin(&d, 2); ac(&d, 0); ms(&d, 1, 0, 16, 0x01, 0x81, 64); cfg_end(&d);
    CHECK(d.n == 581);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(umk_parse_cfg(d.b, 512, &f) == UMK_KIND_NONE);
    CHECK(umk_direct_ok(d.b, d.n, 2, 2, 256) == 0);
    CHECK(umk_direct_ok(d.b, d.n, 2, 2, 1024) == 1);

    /* An interface among the first two with more endpoints than libDaisy's
     * array holds (2): its parser would overrun -- reported as "no". */
    cfg_begin(&d, 2);
    itf(&d, 0, 0, 3, 0x01, 0x01, 0x00);
    ep7(&d, 0x83, 0x03, 8, 1); ep7(&d, 0x84, 0x03, 8, 1); ep7(&d, 0x85, 0x03, 8, 1);
    ms(&d, 1, 0, 1, 0x01, 0x81, 64);
    cfg_end(&d);
    CHECK(umk_parse_cfg(d.b, d.n, &f) == UMK_KIND_MIDI);
    CHECK(direct(&d) == 0);

    /* Garbage: a zero-length descriptor stops the walk. */
    {
        const uint8_t bad[12] = {9, 2, 12, 0, 1, 1, 0, 0x80, 50, 0, 4, 0};
        CHECK(umk_parse_cfg(bad, sizeof bad, &f) == UMK_KIND_NONE);
        CHECK(umk_direct_ok(bad, sizeof bad, 2, 2, 256) == 0);
    }
}

/* ---------------------------------------------------------------- roles */

static void test_roles(void)
{
    CHECK(umk_role(0x1235, 0x0113, UMK_KIND_MIDI) == UMK_ROLE_MINI);
    CHECK(umk_role(0x1235, 0x0061, UMK_KIND_MIDI) == UMK_ROLE_XL);
    CHECK(umk_role(0x1235, 0x0102, UMK_KIND_MIDI) == UMK_ROLE_KEYS); /* a Launchkey: keys */
    CHECK(umk_role(0x0582, 0x0156, UMK_KIND_MIDI) == UMK_ROLE_KEYS); /* any vendor */
    CHECK(umk_role(0x045E, 0x028E, UMK_KIND_XINPUT) == UMK_ROLE_PAD);
    CHECK(umk_role(0x0BDA, 0x8153, UMK_KIND_NONE) == UMK_ROLE_NONE); /* Ethernet */
    CHECK(umk_role(0x1235, 0x0113, UMK_KIND_NONE) == UMK_ROLE_NONE);
}

/* ---------------------------------------------------------------- packets */

static int msg_is(const umk_msg_t *m, uint8_t a, uint8_t b, uint8_t c)
{
    return m->b[0] == a && m->b[1] == b && m->b[2] == c;
}

static void test_packets(void)
{
    umk_keys_t k;
    umk_msg_t  m[32];
    umk_keys_reset(&k);

    /* note on, cable 0 channel 1 */
    {
        const uint8_t p[] = {0x09, 0x90, 60, 100};
        CHECK(umk_keys_rx(&k, p, 4, m, 32) == 1 && msg_is(&m[0], 0x90, 60, 100));
    }
    /* omni: cable 2, channel 5 -> channel 1; the same note again is dropped;
     * a padding packet and a SysEx and a clock byte in the same transfer
     * are ignored */
    {
        const uint8_t p[] = {0x29, 0x94, 64, 80,   0x09, 0x90, 60, 90,
                             0x00, 0x00, 0x00, 0x00, 0x04, 0xF0, 0x7E, 0x7F,
                             0x07, 0x06, 0x01, 0xF7, 0x0F, 0xF8, 0x00, 0x00};
        CHECK(umk_keys_rx(&k, p, sizeof p, m, 32) == 1 && msg_is(&m[0], 0x90, 64, 80));
    }
    CHECK(umk_keys_held_count(&k) == 2);
    /* note-on velocity 0 = off; off for a note never held is dropped; real
     * note-off on another channel */
    {
        const uint8_t p[] = {0x09, 0x90, 60, 0, 0x08, 0x80, 61, 0x40, 0x08, 0x8F, 64, 0x40};
        const int     n   = umk_keys_rx(&k, p, sizeof p, m, 32);
        CHECK(n == 2 && msg_is(&m[0], 0x80, 60, 0) && msg_is(&m[1], 0x80, 64, 0));
    }
    CHECK(umk_keys_held_count(&k) == 0);
    /* sustain: edges only, other CCs and pitch bend dropped */
    {
        const uint8_t p[] = {0x0B, 0xB0, 64, 127, 0x0B, 0xB0, 64, 100, 0x0B, 0xB3, 1, 50,
                             0x0E, 0xE0, 0x00, 0x40, 0x0B, 0xB0, 64, 10};
        const int     n   = umk_keys_rx(&k, p, sizeof p, m, 32);
        CHECK(n == 2 && msg_is(&m[0], 0xB0, 64, 127) && msg_is(&m[1], 0xB0, 64, 0));
        CHECK(k.sustain == 0);
    }
    /* a trailing partial packet is ignored */
    {
        const uint8_t p[] = {0x09, 0x90, 50, 1, 0x09, 0x90};
        CHECK(umk_keys_rx(&k, p, sizeof p, m, 32) == 1 && msg_is(&m[0], 0x90, 50, 1));
    }
    /* data bytes are masked to 7 bits */
    {
        const uint8_t p[] = {0x08, 0x80, 50 | 0x80, 0};
        CHECK(umk_keys_rx(&k, p, 4, m, 32) == 1 && msg_is(&m[0], 0x80, 50, 0));
    }
    /* a full 64-byte transfer of note-ons with room for 4: 4 go, and only
     * those 4 count as held (the rest are not half-remembered) */
    {
        uint8_t p[64];
        for (int i = 0; i < 16; i++)
        {
            p[4 * i] = 0x09; p[4 * i + 1] = 0x90; p[4 * i + 2] = (uint8_t)(40 + i); p[4 * i + 3] = 100;
        }
        CHECK(umk_keys_rx(&k, p, 64, m, 4) == 4);
        CHECK(umk_keys_held_count(&k) == 4);
        CHECK(msg_is(&m[3], 0x90, 43, 100));
    }
    /* unplugged with 4 keys and the pedal down: 4 offs, lowest first, then
     * sustain off -- drained through a small buffer */
    {
        const uint8_t p[] = {0x0B, 0xB0, 64, 127};
        CHECK(umk_keys_rx(&k, p, 4, m, 32) == 1);
        int n = umk_keys_release_all(&k, m, 3);
        CHECK(n == 3 && msg_is(&m[0], 0x80, 40, 0) && msg_is(&m[2], 0x80, 42, 0));
        n = umk_keys_release_all(&k, m, 3);
        CHECK(n == 2 && msg_is(&m[0], 0x80, 43, 0) && msg_is(&m[1], 0xB0, 64, 0));
        CHECK(umk_keys_release_all(&k, m, 3) == 0);
        CHECK(umk_keys_held_count(&k) == 0 && k.sustain == 0);
    }
    /* the extremes of the range */
    {
        const uint8_t p[] = {0x09, 0x90, 0, 1, 0x09, 0x90, 127, 127};
        CHECK(umk_keys_rx(&k, p, sizeof p, m, 32) == 2);
        CHECK(umk_keys_held_count(&k) == 2);
        CHECK(umk_keys_release_all(&k, m, 32) == 2 && msg_is(&m[0], 0x80, 0, 0)
              && msg_is(&m[1], 0x80, 127, 0));
    }
}

int main(void)
{
    test_descriptors();
    test_roles();
    test_packets();
    printf("test_usb_keys: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
