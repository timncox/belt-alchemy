/*
 * tether_sysex -- TETHER's settings over MIDI SysEx (0.3.0), for Loadout on
 * an iPhone or iPad with the Lab plugged in as its "TETHER" USB-MIDI device.
 * No SDK in it (natively tested in ../test).
 *
 * Every message: F0 7D 54 45 <cmd> <data...> F7 (7D: the ID for private use;
 * 54 45: "TE"). Settings travel as the 80-byte state of tether_engine's Pack,
 * each byte as two 4-bit halves, high first (160 data bytes).
 *
 *   to TETHER    01                      ask for the settings
 *                02 <160>                set them all (a whole Pack, check
 *                                        included); answered with 10
 *                03 <layout>             BAND 0, CLOCK 1, CV IN 2 (0.4.0), AUDIO 3
                                        (0.5.0): J3-J10 at once;
 *                                        answered with 10
 *   from TETHER  10 <ver 8> <160>        the settings now: the firmware's
 *                                        version in 8 ASCII bytes (zero padded),
 *                                        then the state
 *                11 <why>                not taken: 1 not a whole state,
 *                                        2 its check or version is wrong,
 *                                        3 a command it doesn't know
 */
#pragma once
#include <stdint.h>
#include <string.h>

#include "tether_engine.h"

namespace tether
{
namespace sysex
{

constexpr uint8_t kId[3] = {0x7D, 0x54, 0x45};
enum : uint8_t
{
    kGet = 0x01, kSet = 0x02, kLayout = 0x03,
    kState = 0x10, kRefused = 0x11,
};
enum : uint8_t { kWhyLength = 1, kWhyCheck = 2, kWhyCommand = 3 };

constexpr int kVerBytes = 8;
constexpr int kStateNibbles = 2 * kStateBytes;
/* F0 + id + cmd + version + state + F7 */
constexpr int kStateMsgBytes = 1 + 3 + 1 + kVerBytes + kStateNibbles + 1;

/* What a request asks for. A request is the SysEx between F0 and F7, as
 * Engine::TakeSysex hands it over. */
struct Request
{
    uint8_t cmd    = 0;
    uint8_t layout = 0;
    uint8_t state[kStateBytes];
    uint8_t why    = 0;   /* nonzero: answer kRefused with it */
};

inline bool Parse(const uint8_t* p, int n, Request* r)
{
    if (n < 4 || p[0] != kId[0] || p[1] != kId[1] || p[2] != kId[2]) return false;
    r->cmd = p[3];
    r->why = 0;
    const uint8_t* d  = p + 4;
    const int      dn = n - 4;
    switch (r->cmd)
    {
        case kGet: break;
        case kLayout:
            if (dn < 1 || d[0] >= kNumLayouts) r->why = kWhyCommand;
            else r->layout = d[0];
            break;
        case kSet:
            if (dn != kStateNibbles) { r->why = kWhyLength; break; }
            for (int i = 0; i < kStateBytes; i++) r->state[i] = (uint8_t)((d[2 * i] & 0x0F) << 4 | (d[2 * i + 1] & 0x0F));
            break;
        default: r->why = kWhyCommand; break;
    }
    return true;
}

/* The answers, whole (F0 .. F7). Returns the length. */
inline int State(const uint8_t state[kStateBytes], const char* version, uint8_t* out)
{
    int k = 0;
    out[k++] = 0xF0;
    for (int i = 0; i < 3; i++) out[k++] = kId[i];
    out[k++] = kState;
    const int vl = (int)strlen(version);
    for (int i = 0; i < kVerBytes; i++) out[k++] = i < vl ? (uint8_t)(version[i] & 0x7F) : 0;
    for (int i = 0; i < kStateBytes; i++)
    {
        out[k++] = (uint8_t)(state[i] >> 4);
        out[k++] = (uint8_t)(state[i] & 0x0F);
    }
    out[k++] = 0xF7;
    return k;
}

inline int Refused(uint8_t why, uint8_t* out)
{
    int k = 0;
    out[k++] = 0xF0;
    for (int i = 0; i < 3; i++) out[k++] = kId[i];
    out[k++] = kRefused;
    out[k++] = why;
    out[k++] = 0xF7;
    return k;
}

/* A SysEx message as USB-MIDI event packets (cable 0): byte 0 the CIN, then
 * up to three bytes. Returns how many packets. */
inline int Packets(const uint8_t* msg, int n, uint8_t (*pk)[4], int cap)
{
    int np = 0;
    for (int i = 0; i < n && np < cap; i += 3)
    {
        const int left = n - i;
        uint8_t*  p    = pk[np++];
        memset(p, 0, 4);
        if (left > 3) { p[0] = 0x4; memcpy(p + 1, msg + i, 3); }
        else
        {
            p[0] = (uint8_t)(0x4 + left);   /* 5, 6, 7: ends with 1, 2, 3 bytes */
            memcpy(p + 1, msg + i, (size_t)left);
        }
    }
    return np;
}

} // namespace sysex
} // namespace tether
