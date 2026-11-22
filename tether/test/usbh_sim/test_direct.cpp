/* TETHER 0.3.1: the FM-1 (and SMK-37) straight on the Lab's front USB-C, and
 * through a powered hub, run through ST's real host core (usbh_core.c,
 * usbh_ctlreq.c, usbh_midi.c) and Belt's hub driver against a modelled
 * device (sim.cpp). The loop below is tether_usbh.cpp's host side: libDaisy's
 * Process (which starts the host again after an abort), the 256-byte clamp,
 * and usbdirect::Watch around it. "0.3.0" runs the same loop without Watch,
 * only moving the MIDI interface, as the module did. */
#include <stdio.h>
#include <string.h>
#include <initializer_list>

#include "sim.h"
#include "usbh_midi.h"
#include "usbh_hub_midi.h"
#include "usbh_direct.h"

static int g_fail = 0;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
    } while (0)

/* ---- the devices ---- */
#include "../fm1_desc.inc"   /* fm1[], smk37[] (the same bytes test_midi_pick checks) */

static const uint8_t kFm1Dev[18]   = {18, 1, 0x10, 0x01, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x03, 1, 2, 3, 1};
static const uint8_t kSmk37Dev[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x03, 1, 2, 3, 1};
static const uint8_t kLang[4]      = {4, 3, 0x09, 0x04};
static const uint8_t kMfr[14]      = {14, 3, 'F', 0, 'e', 0, 'l', 0, 'u', 0, 'c', 0, 'c', 0};
static const uint8_t kProd[10]     = {10, 3, 'F', 0, 'M', 0, '-', 0, '1', 0};
static const uint8_t kSer[10]      = {10, 3, '0', 0, '0', 0, '0', 0, '1', 0};
/* a mouse: HID, no MIDI */
static const uint8_t kMouseDev[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 8, 0x6D, 0x04, 0x01, 0x00, 0, 1, 0, 0, 0, 1};
static const uint8_t kMouseCfg[34] = {9, 2, 34, 0, 1, 1, 0, 0xA0, 50, 9, 4, 0, 0, 1, 3, 1, 2, 0,
                                      9, 0x21, 0x11, 1, 0, 1, 0x22, 52, 0, 7, 5, 0x81, 3, 4, 0, 10};

static SimDev fm1_dev()
{
    SimDev d;
    d.dev_desc = kFm1Dev;
    d.cfg      = fm1;
    d.cfg_len  = sizeof fm1;
    d.strings[0] = kLang; d.strings[1] = kMfr; d.strings[2] = kProd; d.strings[3] = kSer;
    d.midi_in_ep = 0x81;
    return d;
}
static SimDev smk37_dev()
{
    SimDev d = fm1_dev();
    d.dev_desc = kSmk37Dev;
    d.cfg      = smk37;
    d.cfg_len  = sizeof smk37;
    return d;
}

/* ---- the Lab's host side (tether_usbh.cpp, minus the device turns) ---- */
static USBH_HandleTypeDef h;
static usbdirect::Watch   w;
static bool               use_watch = true;
static bool               direct = false;
static uint32_t           rx = 0;
static uint8_t            moved_030 = 0;
static constexpr unsigned kPollsPerMs = 8;

static void on_rx(uint8_t* buf, size_t len, void*)
{
    for (size_t i = 0; i + 3 < len; i += 4)
        if (buf[i] | buf[i + 1] | buf[i + 2] | buf[i + 3]) rx++;
}
static void on_rx_hub(uint8_t dev, uint8_t* buf, size_t len, void*)
{
    if (HUBMIDI_DevKind(dev) == HUBMIDI_KIND_MIDI) on_rx(buf, len, nullptr);
}
static void user(USBH_HandleTypeDef* ph, uint8_t id)
{
    switch (id)
    {
        case HOST_USER_CLASS_ACTIVE:
            if (ph->pActiveClass == USBH_MIDI_CLASS)
            {
                USBH_MIDI_SetReceiveCallback(ph, on_rx, nullptr);
                direct = true;
            }
            break;
        case HOST_USER_DISCONNECTION:
            direct = false;
            moved_030 = 0;
            w.Unplugged();
            break;
        case HOST_USER_UNRECOVERED_ERROR: w.OnError(); break;
        default: break;
    }
}
static void host_start()
{
    memset(&h, 0, sizeof h);
    USBH_Init(&h, user, HOST_HS);
    USBH_RegisterClass(&h, USBH_MIDI_CLASS);
    USBH_RegisterClass(&h, USBH_HUB_MIDI_CLASS);
    HUBMIDI_SetReceiveCallback(on_rx_hub, nullptr);
    USBH_Start(&h);
}
/* libDaisy's USBHostHandle::Process: an abort starts the host again */
static void daisy_process()
{
    if (h.gState == HOST_ABORT_STATE)
    {
        const uint32_t n = h.ClassNumber;
        USBH_Stop(&h);
        USBH_DeInit(&h);
        USBH_Init(&h, user, HOST_HS);
        USBH_Start(&h);
        h.ClassNumber = n;
    }
    else
        USBH_Process(&h);
}
static void poll()
{
    const uint32_t now = sim::Now();
    const bool run = use_watch ? w.Before(&h, now) : true;
    if (run) daisy_process();
    if (h.gState == HOST_ENUMERATION && h.EnumState == ENUM_GET_FULL_CFG_DESC && h.RequestState == CMD_SEND
        && h.device.CfgDesc.wTotalLength > USBH_MAX_SIZE_CONFIGURATION)
        h.device.CfgDesc.wTotalLength = USBH_MAX_SIZE_CONFIGURATION;
    if (use_watch) w.After(&h, now);
    else if (h.gState == HOST_CHECK_CLASS && !moved_030) moved_030 = usbdirect::place_midi(&h);
    /* the module's control loop comes round many times a millisecond */
    static unsigned n = 0;
    if (++n % kPollsPerMs == 0) sim::Advance(&h, 1);
}
/* the Lab's B3: 3 green, 2 red, 1 amber (as midihost::Stage) */
static uint8_t stage()
{
    if (direct && h.pActiveClass == USBH_MIDI_CLASS) return 3;
    if (h.pActiveClass == USBH_HUB_MIDI_CLASS && h.gState == HOST_CLASS)
    {
        for (uint8_t d = 0; d < HUBMIDI_MAX_DEVICES; d++)
            if (HUBMIDI_DevReady(&h, d) && HUBMIDI_DevKind(d) == HUBMIDI_KIND_MIDI) return 3;
        return 1;
    }
    if (use_watch && w.Failed()) return 2;
    return h.device.is_connected ? 1 : 0;
}
/* run until green (or ms pass); the time it took, or 0 */
static uint32_t run_until_green(uint32_t ms)
{
    const uint32_t t0 = sim::Now();
    while (sim::Now() - t0 < ms)
    {
        poll();
        if (stage() == 3) return sim::Now() - t0 + 1;
    }
    return 0;
}
static void run(uint32_t ms)
{
    const uint32_t t0 = sim::Now();
    while (sim::Now() - t0 < ms) poll();
}
static bool plays()
{
    const uint8_t on[4] = {0x09, 0x90, 60, 100};
    const uint32_t before = rx;
    sim::QueueMidi(on);
    run(20);
    return rx == before + 1;
}
static void fresh(bool watch)
{
    sim::Reset();
    w         = usbdirect::Watch{};
    use_watch = watch;
    direct    = false;
    rx        = 0;
    moved_030 = 0;
    host_start();
    run(50);
}

int main()
{
    /* 1. The FM-1 as it should be, straight on: green, notes arrive. */
    {
        printf("FM-1 straight on, prompt\n");
        fresh(true);
        SimDev d = fm1_dev();
        sim::Plug(&h, &d);
        const uint32_t t = run_until_green(3000);
        CHECK(t > 0, "no green");
        printf("  green after %u ms, MIDI interface %d moved, %u strings asked\n", t, w.moved - 1, d.strings_asked);
        CHECK(w.moved == 4, "interface 3 should be moved (got %d)", w.moved);
        CHECK(plays(), "a note didn't arrive");
        CHECK(w.tries == 0, "%u failed tries", w.tries);
    }
    /* 2. The same with 0.3.0's loop: also green (the descriptor isn't the problem). */
    {
        printf("FM-1 straight on, prompt, 0.3.0\n");
        fresh(false);
        SimDev d = fm1_dev();
        sim::Plug(&h, &d);
        CHECK(run_until_green(3000) > 0, "0.3.0 no green with a prompt FM-1");
        CHECK(plays(), "a note didn't arrive");
    }
    /* 3. Slow to take its address (its USB is looked at every 0.5 ms, later
     *    while its audio is busy). 0.3.0 gets there by luck: the core gives up,
     *    resets, and carries on from where it was at address 0, which the FM-1
     *    happens to accept. 0.3.1 waits for the address and needs no second try. */
    for (uint32_t lag : {3u, 6u, 9u})
    {
        printf("FM-1 straight on, address takes %u ms\n", lag);
        fresh(false);
        SimDev d0 = fm1_dev();
        d0.addr_lag_ms = lag;
        sim::Plug(&h, &d0);
        const uint32_t t0 = run_until_green(5000);
        printf("  0.3.0: green after %u ms, %u bus resets\n", t0, d0.resets);

        fresh(true);
        SimDev d = fm1_dev();
        d.addr_lag_ms = lag;
        sim::Plug(&h, &d);
        const uint32_t t = run_until_green(5000);
        printf("  0.3.1: green after %u ms, %u bus resets\n", t, d.resets);
        CHECK(t > 0, "0.3.1 no green at %u ms", lag);
        CHECK(w.tries == 0 && d.resets == 1, "0.3.1 needed %u more tries", w.tries);
        CHECK(plays(), "a note didn't arrive");
    }
    /* 4. Misses a few requests (busy for a moment), and misses now and then. */
    {
        printf("FM-1 misses its first 7 requests, then 1 in 5\n");
        fresh(true);
        SimDev d = fm1_dev();
        d.drop_first = 7;
        d.drop_every = 5;
        sim::Plug(&h, &d);
        const uint32_t t = run_until_green(8000);
        printf("  green after %u ms, %u failed tries, %u setups\n", t, w.tries, d.setups);
        CHECK(t > 0, "no green");
        CHECK(plays(), "a note didn't arrive");
    }
    /* 5. Slow out of the bus reset: deaf 40 ms past the core's 100 ms wait. */
    {
        printf("FM-1 deaf 140 ms after each bus reset\n");
        fresh(true);
        SimDev d = fm1_dev();
        d.deaf_after_reset_ms = 140;
        sim::Plug(&h, &d);
        const uint32_t t = run_until_green(5000);
        printf("  green after %u ms\n", t);
        CHECK(t > 0, "no green");
    }
    /* 6. Pulled up but never answering (its USB not running): red after three
     *    tries, one blink (no answer), still trying; then it wakes: green. */
    {
        printf("FM-1 that never answers, then wakes\n");
        fresh(true);
        SimDev d = fm1_dev();
        d.dead = true;
        sim::Plug(&h, &d);
        run(4000);
        printf("  stage %u, where %u, %u tries, %u bus resets\n", stage(), w.where, w.tries, d.resets);
        CHECK(stage() == 2, "should be red (stage %u)", stage());
        CHECK(w.where == usbdirect::kNoAnswer, "where %u", w.where);
        CHECK(d.resets >= 3, "should keep trying (%u resets)", d.resets);
        d.dead = false;
        const uint32_t t = run_until_green(4000);
        printf("  woke: green after %u ms\n", t);
        CHECK(t > 0, "no green after waking");
        CHECK(w.fails == 0 && w.where == 0, "red cleared");

        /* 0.3.0 the same: red too, but nothing says why */
        fresh(false);
        SimDev d0 = fm1_dev();
        d0.dead = true;
        sim::Plug(&h, &d0);
        run(4000);
        printf("  0.3.0: %u bus resets, B3 %s\n", d0.resets, direct ? "green" : "red");
    }
    /* 7. Unplugged while red: dim; plugged in again: green. */
    {
        printf("Unplug while red, plug a good one\n");
        fresh(true);
        SimDev bad = fm1_dev();
        bad.dead = true;
        sim::Plug(&h, &bad);
        run(4000);
        CHECK(stage() == 2, "red");
        sim::Unplug(&h);
        run(100);
        CHECK(stage() == 0, "dim after unplug (stage %u)", stage());
        SimDev d = fm1_dev();
        sim::Plug(&h, &d);
        CHECK(run_until_green(3000) > 0, "no green");
    }
    /* 8. The SMK-37 straight on (MIDI is interface 1 already). */
    {
        printf("SMK-37 straight on, address takes 6 ms\n");
        fresh(true);
        SimDev d = smk37_dev();
        d.addr_lag_ms = 6;
        sim::Plug(&h, &d);
        const uint32_t t = run_until_green(5000);
        printf("  green after %u ms\n", t);
        CHECK(t > 0, "no green");
        CHECK(w.moved == 0, "nothing to move");
        CHECK(plays(), "a note didn't arrive");
    }
    /* 9. Not MIDI (a mouse): red, three blinks, keeps out of the way. */
    {
        printf("A mouse\n");
        fresh(true);
        SimDev d;
        d.dev_desc = kMouseDev;
        d.cfg      = kMouseCfg;
        d.cfg_len  = sizeof kMouseCfg;
        sim::Plug(&h, &d);
        run(4000);
        printf("  stage %u, where %u\n", stage(), w.where);
        CHECK(stage() == 2, "red");
        CHECK(w.where == usbdirect::kSetup, "where %u", w.where);
    }
    /* 10. Through a powered hub, FM-1 and SMK-37, prompt and slow: as before. */
    for (int which = 0; which < 2; which++)
        for (uint32_t lag : {0u, 4u})
            for (int watch = 0; watch < 2; watch++)
            {
                printf("%s through a hub, address takes %u ms, %s\n", which ? "SMK-37" : "FM-1", lag,
                       watch ? "0.3.1" : "0.3.0");
                fresh(watch);
                SimDev d = which ? smk37_dev() : fm1_dev();
                d.addr_lag_ms = lag;
                sim::Plug(&h, &d, true);
                const uint32_t t = run_until_green(5000);
                printf("  green after %u ms\n", t);
                CHECK(t > 0, "no green through the hub");
                CHECK(plays(), "a note didn't arrive through the hub");
                CHECK(w.tries == 0, "%u failed tries", w.tries);
            }
    /* 11. Green stays green: a minute of notes, nothing restarted. */
    {
        printf("FM-1 straight on, a minute of playing\n");
        fresh(true);
        SimDev d = fm1_dev();
        d.addr_lag_ms = 6;
        sim::Plug(&h, &d);
        CHECK(run_until_green(5000) > 0, "no green");
        const uint32_t r0 = w.restarts;
        int ok = 0;
        for (int i = 0; i < 600; i++) { ok += plays(); run(80); }
        CHECK(ok == 600, "%d of 600 notes", ok);
        CHECK(w.restarts == r0 && stage() == 3, "restarted or lost");
    }

    /* 12. For the record: what else 0.3.0 itself got through, straight on. */
    {
        printf("0.3.0 straight on (for the record)\n");
        struct { const char* what; uint32_t deaf, drop_first, drop_every; } cases[] = {
            {"misses its first 7 requests, then 1 in 5", 0, 7, 5},
            {"deaf 140 ms after each bus reset", 140, 0, 0},
            {"misses 1 request in 3", 0, 0, 3},
        };
        for (auto& c : cases)
        {
            fresh(false);
            SimDev d = fm1_dev();
            d.deaf_after_reset_ms = c.deaf;
            d.drop_first = c.drop_first;
            d.drop_every = c.drop_every;
            sim::Plug(&h, &d);
            const uint32_t t0 = run_until_green(8000);
            fresh(true);
            SimDev d1 = fm1_dev();
            d1.deaf_after_reset_ms = c.deaf;
            d1.drop_first = c.drop_first;
            d1.drop_every = c.drop_every;
            sim::Plug(&h, &d1);
            const uint32_t t1 = run_until_green(8000);
            printf("  %s: 0.3.0 %s (%u ms), 0.3.1 %s (%u ms)\n", c.what, t0 ? "green" : "never green", t0,
                   t1 ? "green" : "never green", t1);
            CHECK(t1 > 0, "0.3.1 never green: %s", c.what);
        }
    }

    printf(g_fail ? "usbh_sim: %d FAILED\n" : "usbh_sim: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
