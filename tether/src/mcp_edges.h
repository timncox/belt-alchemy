/* mcp_edges -- every hit and clock pulse on J3-J6 reaches the jack (0.3.0).
 *
 * J3-J6 come out of the Lab's I2C DAC, written from the main loop's 1 ms
 * poll. That loop stops for a few milliseconds every frame while the screen
 * and the lights are drawn, so a drum's 3 ms drop between two hits, or a
 * whole short pulse, could fall in a stop and never reach the jack: in the
 * emulator about one hat in twenty on J5 ran into the last one. The audio
 * side counts each rise; the poll then makes sure each one is seen as a low
 * and then the hit, 3 ms each at least (as the engine's own drop between two
 * hits), late if it must be.
 *
 * Only for jobs that are on/off (gates, triggers, clocks, accents, reset,
 * run). Pitch, envelopes, velocity and section are written as they are.
 * Plain C++, no SDK: tested on a computer (../test/test_edges.cpp). */
#pragma once
#include <stdint.h>

namespace mcpedge
{

constexpr float    kOn     = 0.5f;   /* above this a jack counts as up */
constexpr uint32_t kHoldMs = 3u;     /* a low or a hit shown late stays at least this long */

/* The audio side's view of one jack. */
struct Feed
{
    uint32_t rises = 0;    /* how many times it went up */
    float    peak  = 0;    /* its height the last time it went up */
    float    now   = 0;    /* its volts now */
    float    prev  = 0;
};

/* Each audio block, after the engine: the jack's volts now. */
inline void Note(Feed& f, float v)
{
    if (f.prev < kOn && v >= kOn)
    {
        f.peak = v;
        f.rises++;
    }
    f.prev = v;
    f.now  = v;
}

/* The poll's view of one jack. */
struct Out
{
    float    sent = 99.0f;   /* what the DAC has (99: nothing yet) */
    uint32_t seen = 0;       /* rises dealt with */
    uint8_t  then = 0;       /* 1: holding a low, 2: a hit shown late, a low after it, 3: holding a hit */
    uint32_t at   = 0;       /* when that began (ms) */
    bool     owe  = false;   /* a rise not shown yet */
};

/* What to write now (ms: the poll's clock), from a snapshot of the feed
 * (now, rises, peak). edgy: the jack's job is on/off. Every low and every
 * hit the jack shows lasts kHoldMs at least. */
inline float Next(Out& o, uint32_t ms, float now, uint32_t rises, float peak, bool edgy)
{
    if (!edgy)
    {
        o.seen = rises;
        o.then = 0;
        o.owe  = false;
        return now;
    }
    if (rises != o.seen)
    {
        o.seen = rises;
        o.owe  = true;
    }
    switch (o.then)
    {
        case 1:                                      /* holding a low */
            if (ms - o.at < kHoldMs) return 0.0f;
            o.then = 0;
            break;
        case 2:                                      /* a hit shown late: then a low */
            if (ms - o.at < kHoldMs) return peak;
            o.then = 1;
            o.at   = ms;
            return 0.0f;
        case 3:                                      /* holding a hit */
            if (ms - o.at < kHoldMs) return now >= kOn ? now : peak;
            o.then = 0;
            break;
        default: break;
    }
    if (o.owe)
    {
        if (o.sent >= kOn)                           /* the jack never went down for it */
        {
            o.then = 1;
            o.at   = ms;
            return 0.0f;
        }
        o.owe  = false;
        o.at   = ms;
        o.then = now >= kOn ? 3 : 2;                 /* up now, or it came and went */
        return now >= kOn ? now : peak;
    }
    if (o.sent >= kOn && now < kOn)                  /* it went down: hold that low */
    {
        o.then = 1;
        o.at   = ms;
        return 0.0f;
    }
    return now;
}

} // namespace mcpedge
