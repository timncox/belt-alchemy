/* midi_itf_pick -- find a device's USB-MIDI streaming interface in its raw
 * configuration descriptor, wherever it sits.
 *
 * Why (TETHER 0.1.1): libDaisy's ST host keeps only the first two interface
 * descriptors it meets (USBH_MAX_NUM_INTERFACES = 2, alternate settings
 * counted), and its MIDI class looks for MIDI streaming only there. A
 * composite audio + MIDI device that lists its audio streaming interfaces
 * first never gets its MIDI opened: the FM-1 (PROTECTOR) is audio control 0,
 * audio in 1 (alt 0, 1), audio out 2 (alt 0, 1), MIDI 3, so the host saw
 * audio control and audio in, found no MIDI, gave up and started over, and
 * B3 stayed amber. This finds the MIDI interface in the raw bytes so the
 * firmware can put it where the class looks.
 *
 * Plain C, no ST types, so the computer-side tests can run it. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct
{
    uint8_t itf[9];      /* the interface descriptor, as sent */
    uint8_t neps;        /* endpoints taken, at most 2 */
    uint8_t ep[2][7];    /* their descriptors' first 7 bytes */
} midi_pick_t;

/* cfg: the configuration descriptor as read; len: how many bytes of it are
 * really there. Returns 1 with *out filled when an interface of class 1
 * (audio), subclass 3 (MIDI streaming), alternate 0 with a bulk IN endpoint
 * is found; 0 otherwise. Stops at a malformed descriptor. */
static inline int midi_itf_pick(const uint8_t* cfg, uint16_t len, midi_pick_t* out)
{
    uint16_t p = 0;
    int      in_midi = 0, has_in = 0;
    memset(out, 0, sizeof *out);
    if (len < 9 || cfg[1] != 2) return 0;
    p = cfg[0];
    while (p + 2u <= len)
    {
        const uint8_t l = cfg[p], t = cfg[p + 1];
        if (l < 2 || p + l > len) break;
        if (t == 4 && l >= 9)
        {
            if (in_midi) break;                                     /* the next interface: done */
            if (cfg[p + 5] == 1 && cfg[p + 6] == 3 && cfg[p + 3] == 0)
            {
                memcpy(out->itf, cfg + p, 9);
                in_midi = 1;
            }
        }
        else if (t == 5 && l >= 7 && in_midi && out->neps < 2 && (cfg[p + 3] & 3u) == 2u)
        {
            memcpy(out->ep[out->neps++], cfg + p, 7);
            if (cfg[p + 2] & 0x80u) has_in = 1;
        }
        p = (uint16_t)(p + l);
    }
    if (!in_midi || !has_in) return 0;
    out->itf[4] = out->neps;   /* only the bulk endpoints taken are counted */
    return 1;
}
