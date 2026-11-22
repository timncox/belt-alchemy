/* mcp_edges: hits on J3-J6 survive the main loop's stops. The audio side runs
 * every 0.5 ms; the poll every 1 ms, but not during a 6 ms stop each 16 ms
 * frame (the screen and lights), as on the Lab. */
#include <stdio.h>
#include <vector>
#include "mcp_edges.h"

static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("FAIL   " __VA_ARGS__); fails++; } printf("\n"); } while (0)

struct Run { int sig_rises = 0, dac_rises = 0, min_low = 999, min_high = 999, extra = 0; };

/* sig(t_halfms) -> volts; ms long. Stop: polls skip ms 10..15 of each 16. */
template <class F> static Run run(F sig, int ms, bool edgy, int stop = 6)
{
    using namespace mcpedge;
    Feed f; Out o; Run r;
    float dac = 0, sprev = 0; int since = 0; bool up = false;
    for (int h = 0; h < 2 * ms; h++)
    {
        const float v = sig(h);
        if (sprev < kOn && v >= kOn) r.sig_rises++;
        sprev = v;
        Note(f, v);
        if (h % 2 == 0 && (h / 2) % 16 < 16 - stop)
        {
            const float w = Next(o, (uint32_t)(h / 2), f.now, f.rises, f.peak, edgy);
            o.sent = w;
            const bool nu = w >= kOn;
            if (nu != up)
            {
                if (up && since < r.min_high) r.min_high = since;
                if (!up && since < r.min_low && r.dac_rises) r.min_low = since;
                if (nu) r.dac_rises++;
                up = nu; since = 0;
            }
            dac = w;
        }
        since++;
    }
    (void)dac;
    return r;
}

int main()
{
    printf("mcp_edges\n");
    /* hats: held 122 ms, the next one starting as it ends (the drop is 3 ms) */
    auto hats = [](int h) { int t = h % 250; return t < 6 ? 0.0f : 3.5f; };
    Run r = run(hats, 4000, true);
    CHECK(r.dac_rises == r.sig_rises, "held hits with a 3 ms drop: %d at the jack for %d", r.dac_rises, r.sig_rises);
    CHECK(r.min_low >= 6, "each low at the jack lasts at least 3 ms (%d half-ms)", r.min_low);
    /* without it: the drops that fall in a stop are lost */
    {
        int lost = 0; float sent = 0; bool up = false; int sr = 0, dr = 0; float sp = 0;
        for (int h = 0; h < 8000; h++) { float v = hats(h); if (sp < 0.5f && v >= 0.5f) sr++; sp = v;
            if (h % 2 == 0 && (h / 2) % 16 < 10) { sent = v; bool nu = sent >= 0.5f; if (nu && !up) dr++; up = nu; } }
        lost = sr - dr;
        CHECK(lost > 0, "(written as they are, %d of %d would be lost)", lost, sr);
    }
    /* 5 ms clock pulses every 125 ms: none lost, even inside a stop */
    auto clk = [](int h) { return h % 250 < 10 ? 5.0f : 0.0f; };
    r = run(clk, 4000, true);
    CHECK(r.dac_rises == r.sig_rises, "5 ms pulses: %d at the jack for %d", r.dac_rises, r.sig_rises);
    /* a 2 ms pulse wholly inside a stop is still shown */
    auto short_ = [](int h) { int t = h % 64; return (t >= 22 && t < 26) ? 5.0f : 0.0f; };   /* ms 11-13 of 32 */
    r = run(short_, 3200, true);
    CHECK(r.dac_rises == r.sig_rises && r.min_high >= 6, "2 ms pulses inside a stop: %d for %d, each up at least 3 ms", r.dac_rises, r.sig_rises);
    /* 24 a beat at 240 BPM: 5 ms up, 5.4 ms down, none lost */
    auto c24 = [](int h) { return h < 7900 && (h * 10) % 208 < 100 ? 5.0f : 0.0f; };   /* stops 50 ms before the end */
    r = run(c24, 4000, true);
    CHECK(r.dac_rises == r.sig_rises && r.dac_rises > 300, "24 a beat at 240 BPM: %d for %d", r.dac_rises, r.sig_rises);
    /* a gate held for seconds: one rise, no drop */
    auto held = [](int h) { return h > 20 ? 5.0f : 0.0f; };
    r = run(held, 3000, true);
    CHECK(r.dac_rises == 1, "a held gate: %d rise, no drops", r.dac_rises);
    /* pitch (not on/off): written as it is */
    auto pitch = [](int h) { return (h / 100) % 2 ? 1.0f : 0.2f; };
    r = run(pitch, 2000, false);
    CHECK(r.dac_rises == r.sig_rises, "pitch and the like: passed through (%d for %d)", r.dac_rises, r.sig_rises);
    /* no stops at all: the same count, nothing added */
    r = run(hats, 4000, true, 0);
    CHECK(r.dac_rises == r.sig_rises, "with no stops: %d for %d", r.dac_rises, r.sig_rises);
    printf(fails ? "FAILED %d\n" : "all passed\n", fails);
    return fails != 0;
}
