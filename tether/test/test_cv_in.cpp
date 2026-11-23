/* Native tests for cv_in.h (0.4.0): a CV input's voltage as PROTECTOR's
 * effect CCs, polled at 1 kHz as the firmware's poll does. */
#include <cstdio>

#include "cv_in.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (cond) g_pass++;                                               \
        else {                                                            \
            g_fail++;                                                     \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                     \
            std::printf("\n");                                            \
        }                                                                 \
    } while (0)

/* Poll `ms` at 1 kHz with a steady voltage; the last value sent (-1 none)
 * and how many went out. */
struct Run { int last = -1, sent = 0; bool release = false; };
static Run poll(cvin::Jack& j, int target, float v, uint32_t& now, int ms)
{
    Run r;
    for (int i = 0; i < ms; i++, now++)
    {
        bool rel = false;
        const int x = cvin::Step(j, target, v, now, &rel);
        if (rel) r.release = true;
        if (x >= 0) { r.last = x; r.sent++; }
    }
    return r;
}

static void test_map()
{
    CHECK(cvin::Value(0, 0.0f) > 63.0f && cvin::Value(0, 0.0f) < 64.0f, "FILTER: 0 V the middle (off)");
    CHECK(cvin::Value(0, -5.0f) == 0.0f && cvin::Value(0, 5.0f) == 127.0f, "FILTER: -5 V to +5 V the whole way");
    CHECK(cvin::Value(1, 0.0f) == 0.0f && cvin::Value(1, 5.0f) == 127.0f && cvin::Value(1, -3.0f) == 0.0f,
          "REVERB: 0 to 5 V, below 0 stays 0");
    CHECK(cvin::Value(13, 9.0f) == 127.0f, "clamped");
    CHECK(cvin::Cc(0) == 102 && cvin::Cc(13) == 115, "CC 102 FILTER .. 115 MORPH");
    uint8_t p[4];
    cvin::Packet(p, 104, 90);
    CHECK(p[0] == 0x0B && p[1] == 0xBF && p[2] == 104 && p[3] == 90, "a control change on channel 16");
}

static void test_arm()
{
    cvin::Jack j;
    uint32_t   now = 0;
    Run r = poll(j, 1, 0.0f, now, 500);
    CHECK(r.sent == 0, "nothing in the jack: nothing sent (%d)", r.sent);
    r = poll(j, 1, 0.1f, now, 500);
    CHECK(r.sent == 0, "a little noise isn't a cable");
    r = poll(j, 1, 2.5f, now, 200);
    CHECK(r.sent >= 1 && r.last >= 63 && r.last <= 64, "2.5 V: about half (%d)", r.last);
    r = poll(j, 1, 0.0f, now, 300);
    CHECK(r.last == 0, "back to 0 V once patched: 0 (%d)", r.last);
}

static void test_steady_and_rate()
{
    cvin::Jack j;
    uint32_t   now = 0;
    poll(j, 0, 1.0f, now, 200);
    Run r = poll(j, 0, 1.0f, now, 1000);
    CHECK(r.sent == 0, "a steady voltage sends nothing more (%d)", r.sent);
    /* jitter of a hundredth of a volt (well under a step) */
    int sent = 0;
    for (int i = 0; i < 1000; i++, now++)
    {
        bool rel = false;
        if (cvin::Step(j, 0, 1.0f + (i & 1 ? 0.01f : -0.01f), now, &rel) >= 0) sent++;
    }
    CHECK(sent == 0, "jitter sends nothing (%d)", sent);
    /* a fast LFO: at most one every kGapMs */
    sent = 0;
    for (int i = 0; i < 1000; i++, now++)
    {
        bool  rel = false;
        const float v = (float)((i % 100) - 50) / 10.0f;
        if (cvin::Step(j, 0, v, now, &rel) >= 0) sent++;
    }
    CHECK(sent > 50 && sent <= 1000 / (int)cvin::kGapMs, "an LFO follows, no faster than every %u ms (%d)",
          (unsigned)cvin::kGapMs, sent);
    /* the ends land exactly */
    Run top = poll(j, 0, 5.0f, now, 300);
    CHECK(top.last == 127, "+5 V reaches 127 (%d)", top.last);
    Run bot = poll(j, 0, -5.0f, now, 300);
    CHECK(bot.last == 0, "-5 V reaches 0 (%d)", bot.last);
}

static void test_job_change()
{
    cvin::Jack j;
    uint32_t   now = 0;
    poll(j, 2, 3.0f, now, 200);
    Run r = poll(j, -1, 3.0f, now, 10);
    CHECK(r.release && r.sent == 0, "no longer an input: the effect goes back to its knob");
    r = poll(j, -1, 3.0f, now, 10);
    CHECK(!r.release, "once");
    cvin::Jack k;
    r = poll(k, 2, 0.0f, now, 10);
    r = poll(k, 5, 0.0f, now, 10);
    CHECK(!r.release, "never sent anything: nothing to give back");
    r = poll(k, 5, 4.0f, now, 100);
    CHECK(r.sent >= 1, "the new job sends");
    /* the link comes back: the value again, even unchanged */
    cvin::Resend(k);
    r = poll(k, 5, 4.0f, now, 100);
    CHECK(r.sent == 1, "sent again after the link (%d)", r.sent);
}

int main()
{
    test_map();
    test_arm();
    test_steady_and_rate();
    test_job_change();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
