/*
 * cv_in -- (0.4.0) a CV input on J3..J8 steering PROTECTOR's effects.
 *
 * A jack set to one of the CV jobs (kFnCvIn1.., tether_engine.h) becomes an
 * input. Its voltage goes out of the front USB-C as the CC that PROTECTOR's
 * effects pad already plays (CC 102..115, any channel), to whatever has the
 * Lab as a USB-MIDI device: the FM-1 running the link (CLOCK OUT on LAB) or a
 * computer. CC 116 0 hands every effect back to its knob.
 *
 *   FILTER: 0 V is the middle (64, the filter off); -5 V closes it to 0,
 *           +5 V opens it to 127.
 *   Every other effect: 0 V is 0, +5 V is 127 (below 0 V stays 0).
 *
 * A jack sends nothing until it first reads more than kArmV away from 0 V,
 * so a jack set to CV with nothing in it leaves PROTECTOR's knob alone.
 * After that it sends whenever the value moves by most of a step, at most
 * every kGapMs.
 *
 * No SDK in it: natively tested in ../test/test_cv_in.cpp.
 */
#pragma once
#include <math.h>
#include <stdint.h>

namespace cvin
{

constexpr int      kTargets   = 14;      /* PROTECTOR's pad: FILTER .. MORPH */
constexpr uint8_t  kCc0       = 102;     /* the first one's CC */
constexpr uint8_t  kCcTouch   = 116;     /* 0: every effect back to its knob */
constexpr uint8_t  kChan      = 15;      /* channel 16, away from PROTECTOR's parts (1..8) */
constexpr float    kArmV      = 0.2f;
constexpr uint32_t kGapMs     = 5;
constexpr float    kStep      = 0.75f;   /* of one CC step: a new value only past this */
constexpr float    kSmooth    = 0.35f;   /* one-pole, per poll */

inline uint8_t Cc(int target) { return (uint8_t)(kCc0 + target); }

/* The CC value, 0..127 (as a float, before rounding), for a target's volts. */
inline float Value(int target, float v)
{
    float x = target == 0 ? 63.5f + v * (63.5f / 5.0f) : v * (127.0f / 5.0f);
    return x < 0.0f ? 0.0f : x > 127.0f ? 127.0f : x;
}

struct Jack
{
    int8_t   target = -1;     /* what it was last set to: -1 not a CV input */
    bool     armed  = false;
    float    sm     = 0.0f;   /* smoothed volts */
    int16_t  sent   = -1;     /* last value sent, -1 none since armed / linked */
    uint32_t at     = 0;      /* when */
};

/* One poll for one jack. target: -1 when the jack isn't a CV input now.
 * Returns the value to send now (0..127), or -1 for nothing.
 * *release is set when the jack had been steering an effect and stopped
 * (its job changed): the caller sends CC 116 0 then. */
inline int Step(Jack& j, int target, float volts, uint32_t now, bool* release)
{
    if (target != j.target)
    {
        if (j.armed && j.sent >= 0) *release = true;
        j.target = (int8_t)target;
        j.armed  = false;
        j.sent   = -1;
        j.sm     = volts;
    }
    if (target < 0) return -1;
    j.sm += (volts - j.sm) * kSmooth;
    if (!j.armed)
    {
        if (fabsf(volts) < kArmV) return -1;
        j.armed = true;
        j.sm    = volts;
    }
    const float x = Value(target, j.sm);
    if (j.sent >= 0)
    {
        if (now - j.at < kGapMs) return -1;
        const float d = x - (float)j.sent;
        if (d < kStep && d > -kStep)
        {
            /* the ends always land exactly */
            if (!((x <= 0.0f && j.sent != 0) || (x >= 127.0f && j.sent != 127))) return -1;
        }
    }
    int v = (int)lroundf(x);
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (v == j.sent) return -1;
    j.sent = (int16_t)v;
    j.at   = now;
    return v;
}

/* The link went away (or came back): send every armed jack's value again. */
inline void Resend(Jack& j) { j.sent = -1; }

/* A USB-MIDI event packet: cable 0, CIN B (control change). */
inline void Packet(uint8_t out[4], uint8_t cc, uint8_t v)
{
    out[0] = 0x0B;
    out[1] = (uint8_t)(0xB0 | kChan);
    out[2] = cc;
    out[3] = v;
}

} // namespace cvin
