/* usbrole: the front USB-C taking turns as host and device. */
#include <stdio.h>
#include "usb_role.h"

static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("FAIL   " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* run from t0 to t1 in 1 ms polls; far(t, mode) says whether the far end is
 * seen; returns the number of switches */
template <class F> static int run(usbrole::Role& r, uint32_t t0, uint32_t t1, F far)
{
    int n = 0;
    for (uint32_t t = t0; t != t1; t++) n += r.Step(t, far(t, r.mode));
    return n;
}

int main()
{
    using namespace usbrole;
    printf("usb_role\n");
    Role r;
    r.Start(0);
    int n = run(r, 0, 1499, [](uint32_t, Mode) { return false; });
    CHECK(n == 0 && r.mode == kHost, "nothing plugged in: host for 1.5 s");
    n = run(r, 1499, 1501, [](uint32_t, Mode) { return false; });
    CHECK(n == 1 && r.mode == kDevice, "then a device");
    n = run(r, 1501, 4001, [](uint32_t, Mode) { return false; });
    CHECK(n == 1 && r.mode == kHost, "no computer for 2.5 s: host again");

    /* a keyboard plugged in while the port is a device: found within one turn */
    r.Start(0);
    uint32_t found = 0;
    run(r, 0, 2000, [](uint32_t, Mode) { return false; });   /* device since 1500 */
    for (uint32_t t = 2000; t < 10000 && !found; t++)
        if (!r.Step(t, r.mode == kHost) && r.mode == kHost && r.held) found = t;
    CHECK(r.mode == kHost && found && found <= 4001, "keyboard plugged in mid-turn: host by %u ms (at most one device turn)", found);
    n = run(r, found, found + 60000, [](uint32_t, Mode m) { return m == kHost; });
    CHECK(n == 0 && r.mode == kHost, "keyboard stays: host for a minute, no switching");

    /* a computer: the port becomes a device and stays */
    r.Start(0);
    n = run(r, 0, 60000, [](uint32_t t, Mode m) { return m == kDevice && t > 1700; });
    CHECK(n == 1 && r.mode == kDevice, "computer: device, and stays (%d switch)", n);

    /* the computer unplugged: back to taking turns, a keyboard found next */
    n = run(r, 60000, 61499, [](uint32_t, Mode) { return false; });
    CHECK(n == 0 && r.mode == kDevice, "computer gone: still a device for 1.5 s");
    n = run(r, 61499, 61501, [](uint32_t, Mode) { return false; });
    CHECK(n == 1 && r.mode == kHost, "then a host again");

    /* a keyboard that drops for a moment (a replug) doesn't flip the port at once */
    r.Start(0);
    run(r, 0, 1000, [](uint32_t, Mode m) { return m == kHost; });
    n = run(r, 1000, 2400, [](uint32_t, Mode) { return false; });
    CHECK(n == 0 && r.mode == kHost, "keyboard out for 1.4 s: still a host");

    /* the clock wrapping */
    r.Start(0xFFFFFF00u);
    n = run(r, 0xFFFFFF00u, 0xFFFFFF00u + 1600u, [](uint32_t, Mode) { return false; });
    CHECK(n == 1 && r.mode == kDevice, "across the ms counter's wrap");
    printf(fails ? "FAILED %d\n" : "all passed\n", fails);
    return fails != 0;
}
