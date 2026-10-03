/* usb_midi_keys -- see usb_midi_keys.h. */
#include "usb_midi_keys.h"

#include <string.h>

#define DESC_CONFIG    0x02U
#define DESC_INTERFACE 0x04U
#define DESC_ENDPOINT  0x05U
#define EP_BULK        0x02U
#define EP_INTERRUPT   0x03U

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int is_midi(const uint8_t *itf) { return itf[5] == 0x01 && itf[6] == 0x03; }
static int is_xinput(const uint8_t *itf)
{
    return itf[5] == 0xFF && itf[6] == 0x5D && itf[7] == 0x01;
}

/* ---------------------------------------------------------------- descriptors */

int umk_parse_cfg(const uint8_t *c, uint16_t total, umk_cfg_t *out)
{
    int      cur = UMK_KIND_NONE; /* what the interface being walked is */
    uint16_t i   = 0;
    memset(out, 0, sizeof *out);
    out->interval = 1;
    while (i + 2 <= total)
    {
        const uint8_t len = c[i], type = c[i + 1];
        if (len < 2 || i + len > total) break;
        if (type == DESC_INTERFACE && len >= 9)
        {
            /* The interface taken has its endpoints: done. (Its next
             * alternate setting is a different interface as far as this
             * host goes -- nothing ever selects it.) */
            if (cur != UMK_KIND_NONE && out->in_ep) break;
            /* An interface that gave an OUT but no IN does not count. */
            out->out_ep = 0;
            out->out_size = 0;
            /* MIDI: alternate setting 0 only, the one active without a
             * SET_INTERFACE (USB-MIDI 2.0 keeps MIDI 1.0 there). */
            if (is_midi(&c[i]) && c[i + 3] == 0)
                cur = UMK_KIND_MIDI;
            else if (is_xinput(&c[i]))
                cur = UMK_KIND_XINPUT;
            else
                cur = UMK_KIND_NONE;
            out->iface = c[i + 2];
        }
        else if (type == DESC_ENDPOINT && len >= 7 && cur != UMK_KIND_NONE)
        {
            const uint8_t  ep   = c[i + 2];
            const uint8_t  attr = c[i + 3] & 0x03U;
            const uint16_t mps  = le16(&c[i + 4]) & 0x03FFU;
            const uint8_t  want = cur == UMK_KIND_MIDI ? EP_BULK : EP_INTERRUPT;
            if (attr == want)
            {
                if (ep & 0x80U)
                {
                    if (!out->in_ep)
                    {
                        out->in_ep   = ep;
                        out->in_size = mps;
                        out->kind    = (uint8_t)cur;
                        if (cur == UMK_KIND_XINPUT) out->interval = c[i + 6] ? c[i + 6] : 1;
                    }
                }
                else if (!out->out_ep)
                {
                    out->out_ep   = ep;
                    out->out_size = mps;
                }
            }
        }
        i = (uint16_t)(i + len);
    }
    if (!out->in_ep)
    {
        memset(out, 0, sizeof *out);
        out->interval = 1;
        return UMK_KIND_NONE;
    }
    return out->kind;
}

int umk_direct_ok(const uint8_t *c, uint16_t len, int max_ifaces, int max_eps, uint16_t max_cfg)
{
    if (len < 9 || c[1] != DESC_CONFIG) return 0;
    uint16_t n = le16(&c[2]);
    if (n > len) n = len;
    if (n > max_cfg) n = max_cfg;
    int      seen = 0, ms = 0, ms_seen = 0, ms_in = 0;
    uint16_t i    = 0;
    while (i + 2 <= n && seen <= max_ifaces)
    {
        const uint8_t l = c[i], type = c[i + 1];
        if (l < 2 || i + l > n) break;
        if (type == DESC_INTERFACE && l >= 9)
        {
            if (seen == max_ifaces) break;
            /* The core copies bNumEndpoints endpoints into a max_eps array. */
            if (c[i + 4] > max_eps) return 0;
            if (seen == 0 && c[i + 5] != 0x01) return 0; /* class picked from interface 0 */
            /* USBH_FindInterface: the first MIDIStreaming interface only. */
            ms = !ms_seen && is_midi(&c[i]);
            if (ms) ms_seen = 1;
            seen++;
        }
        else if (type == DESC_ENDPOINT && l >= 7 && ms && (c[i + 2] & 0x80U))
            ms_in = 1;
        i = (uint16_t)(i + l);
    }
    return ms_in;
}

/* ---------------------------------------------------------------- roles */

int umk_role(uint16_t vid, uint16_t pid, int kind)
{
    if (kind == UMK_KIND_XINPUT) return UMK_ROLE_PAD;
    if (kind != UMK_KIND_MIDI) return UMK_ROLE_NONE;
    if (vid == 0x1235 && pid == 0x0113) return UMK_ROLE_MINI;
    if (vid == 0x1235 && pid == 0x0061) return UMK_ROLE_XL;
    return UMK_ROLE_KEYS;
}

/* ---------------------------------------------------------------- keyboard */

static int  held(const umk_keys_t *k, uint8_t n) { return (int)((k->held[n >> 5] >> (n & 31u)) & 1u); }
static void set_held(umk_keys_t *k, uint8_t n, int on)
{
    if (on) k->held[n >> 5] |= 1u << (n & 31u);
    else    k->held[n >> 5] &= ~(1u << (n & 31u));
}

static int emit(umk_msg_t *out, int *n, int cap, uint8_t a, uint8_t b, uint8_t c)
{
    if (*n >= cap) return 0;
    out[*n].b[0] = a;
    out[*n].b[1] = b;
    out[*n].b[2] = c;
    (*n)++;
    return 1;
}

void umk_keys_reset(umk_keys_t *k) { memset(k, 0, sizeof *k); }

int umk_keys_rx(umk_keys_t *k, const uint8_t *buf, size_t len, umk_msg_t *out, int cap)
{
    int n = 0;
    for (size_t i = 0; i + 3 < len; i += 4)
    {
        const uint8_t cin = buf[i] & 0x0FU;
        const uint8_t st  = buf[i + 1] & 0xF0U;
        const uint8_t d1  = buf[i + 2] & 0x7FU;
        const uint8_t d2  = buf[i + 3] & 0x7FU;
        /* CIN says how many bytes; the status byte says what they are. Both
         * must be a note or a CC (CIN 0 is padding, 4-7 SysEx, F single
         * bytes such as clock). */
        if (cin != 0x8 && cin != 0x9 && cin != 0xB) continue;
        if (st == 0xB0)
        {
            if (d1 != 64) continue;
            const uint8_t down = d2 >= 64;
            if (down == k->sustain) continue; /* half-pedal sweeps: edges only */
            if (!emit(out, &n, cap, 0xB0, 64, down ? 127 : 0)) break;
            k->sustain = down;
        }
        else if (st == 0x90 && d2 > 0)
        {
            if (held(k, d1)) continue;
            if (!emit(out, &n, cap, 0x90, d1, d2)) break;
            set_held(k, d1, 1);
        }
        else if (st == 0x80 || st == 0x90)
        {
            if (!held(k, d1)) continue;
            if (!emit(out, &n, cap, 0x80, d1, 0)) break;
            set_held(k, d1, 0);
        }
    }
    return n;
}

int umk_keys_release_all(umk_keys_t *k, umk_msg_t *out, int cap)
{
    int n = 0;
    for (int note = 0; note < 128; note++)
        if (held(k, (uint8_t)note) && emit(out, &n, cap, 0x80, (uint8_t)note, 0))
            set_held(k, (uint8_t)note, 0);
    if (k->sustain && emit(out, &n, cap, 0xB0, 64, 0)) k->sustain = 0;
    return n;
}

int umk_keys_held_count(const umk_keys_t *k)
{
    int c = 0;
    for (int w = 0; w < 4; w++)
        for (uint32_t v = k->held[w]; v; v &= v - 1u) c++;
    return c;
}
