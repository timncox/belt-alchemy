/* lab_link -- see lab_link.h. */
#include "lab_link.h"

#include <string.h>

/* ---------------------------------------------------------------- mapping */

int llk_mini_cell(uint8_t status, uint8_t d1)
{
    const uint8_t st = status & 0xF0;
    if ((status & 0x0F) != 0) return -1; /* channel 1 only */
    const int row = d1 / 10, col = d1 % 10;
    if (st == 0x90 || st == 0x80)
    {
        if (row < 1 || row > 8 || col < 1 || col > 8) return -1;
        return (8 - row) * 8 + (col - 1);
    }
    if (st == 0xB0)
    {
        if (d1 >= 91 && d1 <= 98) return 72 + (d1 - 91);
        if (d1 == 99) return LLK_LOGO;
        if (col == 9 && row >= 1 && row <= 8) return 64 + (8 - row);
    }
    return -1;
}

void llk_mini_msg(int cell, uint8_t* status, uint8_t* d1)
{
    if (cell < 64)       { *status = 0x90; *d1 = (uint8_t)(10 * (8 - cell / 8) + (cell % 8) + 1); }
    else if (cell < 72)  { *status = 0xB0; *d1 = (uint8_t)(10 * (8 - (cell - 64)) + 9); }
    else if (cell < 80)  { *status = 0xB0; *d1 = (uint8_t)(91 + (cell - 72)); }
    else                 { *status = 0xB0; *d1 = 99; }
}

/* ------------------------------------------------------------ USB packets */

static int msg_len(const uint8_t* b, int n)
{
    if (n <= 0 || !(b[0] & 0x80)) return 0;
    if (b[0] == 0xF0)
    {
        for (int i = 1; i < n; i++)
            if (b[i] == 0xF7) return i + 1;
        return 0; /* unterminated */
    }
    if (b[0] >= 0xF8) return 1;
    const uint8_t st = b[0] & 0xF0;
    const int     len = (st == 0xC0 || st == 0xD0) ? 2 : 3;
    return n >= len ? len : 0;
}

int llk_to_usb(const uint8_t* bytes, int n, uint8_t* pk, int cap, int* used)
{
    int i = 0, o = 0;
    while (i < n)
    {
        const int len = msg_len(bytes + i, n - i);
        if (len == 0) { i++; continue; } /* skip stray data bytes */
        const int need = bytes[i] == 0xF0 ? 4 * ((len + 2) / 3) : 4;
        if (o + need > cap) break;
        if (bytes[i] == 0xF0)
        {
            int k = 0;
            while (k < len)
            {
                const int rest = len - k;
                uint8_t*  p    = &pk[o];
                if (rest > 3)
                {
                    p[0] = 0x4;
                    p[1] = bytes[i + k]; p[2] = bytes[i + k + 1]; p[3] = bytes[i + k + 2];
                    k += 3;
                }
                else
                {
                    p[0] = (uint8_t)(rest == 1 ? 0x5 : rest == 2 ? 0x6 : 0x7);
                    p[1] = bytes[i + k];
                    p[2] = rest > 1 ? bytes[i + k + 1] : 0;
                    p[3] = rest > 2 ? bytes[i + k + 2] : 0;
                    k += rest;
                }
                o += 4;
            }
        }
        else if (bytes[i] >= 0xF8)
        {
            pk[o] = 0xF; pk[o + 1] = bytes[i]; pk[o + 2] = 0; pk[o + 3] = 0;
            o += 4;
        }
        else
        {
            pk[o]     = (uint8_t)(bytes[i] >> 4);
            pk[o + 1] = bytes[i];
            pk[o + 2] = bytes[i + 1];
            pk[o + 3] = len > 2 ? bytes[i + 2] : 0;
            o += 4;
        }
        i += len;
    }
    if (used) *used = i;
    return o;
}

int llk_from_usb(const uint8_t* pk, int len, uint8_t* bytes, int cap)
{
    static const uint8_t size[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    int o = 0;
    for (int i = 0; i + 3 < len; i += 4)
    {
        const int s = size[pk[i] & 0xF];
        if (o + s > cap) break;
        for (int k = 0; k < s; k++) bytes[o++] = pk[i + 1 + k];
    }
    return o;
}

/* ----------------------------------------------------------------- parser */

void llk_parse(llk_parser_t* p, const uint8_t* bytes, int n, llk_msg_cb cb, void* ctx)
{
    for (int i = 0; i < n; i++)
    {
        const uint8_t b = bytes[i];
        if (b >= 0xF8) continue; /* realtime: ignored */
        if (b == 0xF0) { p->in_sx = 1; p->n = 0; p->msg[p->n++] = b; continue; }
        if (p->in_sx)
        {
            if (b == 0xF7)
            {
                if (p->n < (int)sizeof p->msg) { p->msg[p->n++] = b; cb(p->msg, p->n, ctx); }
                p->in_sx = 0;
                p->n     = 0;
            }
            else if (b & 0x80) { p->in_sx = 0; p->n = 0; i--; } /* broken SysEx: restart on this status */
            else if (p->n < (int)sizeof p->msg) p->msg[p->n++] = b;
            else p->n = (int)sizeof p->msg + 1;  /* too long: dropped at F7 */
            continue;
        }
        if (b & 0x80)
        {
            p->n      = 0;
            p->msg[0] = b;
            p->n      = 1;
            const uint8_t st = b & 0xF0;
            p->want   = (b >= 0xF0) ? 0 : (st == 0xC0 || st == 0xD0) ? 2 : 3;
            continue;
        }
        if (p->n == 0 || p->want == 0) continue; /* data with no status */
        p->msg[p->n++] = b;
        if (p->n == p->want)
        {
            cb(p->msg, p->n, ctx);
            p->n = 1; /* running status */
        }
    }
}

/* ------------------------------------------------------------------ encode */

static int ctl(uint8_t* out, uint8_t cmd)
{
    out[0] = 0xF0; out[1] = 0x7D; out[2] = 0x4C; out[3] = cmd; out[4] = 0xF7;
    return 5;
}

int llk_enc_hello(uint8_t* out) { return ctl(out, 0x01); }
int llk_enc_hello_reply(uint8_t* out) { return ctl(out, 0x41); }
int llk_enc_release(uint8_t* out) { return ctl(out, 0x02); }

int llk_enc_presence(uint8_t* out, uint8_t bits)
{
    out[0] = 0xF0; out[1] = 0x7D; out[2] = 0x4C; out[3] = 0x03; out[4] = (uint8_t)(bits & 0x7F); out[5] = 0xF7;
    return 6;
}

int llk_enc_pad(uint8_t* out, uint32_t mask)
{
    out[0] = 0xF0; out[1] = 0x7D; out[2] = 0x4C; out[3] = 0x04;
    out[4] = (uint8_t)(mask & 0x7F);
    out[5] = (uint8_t)((mask >> 7) & 0x7F);
    out[6] = (uint8_t)((mask >> 14) & 0x7F);
    out[7] = 0xF7;
    return 8;
}

int llk_enc_mini_led(uint8_t* out, int cell, uint8_t colour)
{
    llk_mini_msg(cell, &out[0], &out[1]);
    out[2] = (uint8_t)(colour & 0x7F);
    return 3;
}

int llk_enc_xl_led(uint8_t* out, int idx, uint8_t colour)
{
    const uint8_t sx[11] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x78, 0x08, (uint8_t)idx, (uint8_t)(colour & 0x7F), 0xF7};
    memcpy(out, sx, sizeof sx);
    return 11;
}

static int is_ctl(const uint8_t* m, int n) { return n >= 5 && m[0] == 0xF0 && m[1] == 0x7D && m[2] == 0x4C; }

/* ================================================================== Lab A */

void llk_host_init(llk_host_t* h)
{
    memset(h, 0, sizeof *h);
    h->presence = LLK_UNKNOWN;
}

void llk_host_attach(llk_host_t* h)
{
    llk_host_init(h);
    h->state = LLK_CANDIDATE;
}

void llk_host_detach(llk_host_t* h) { llk_host_init(h); }

static void host_msg(const uint8_t* m, int n, void* ctx)
{
    llk_host_t* h = (llk_host_t*)ctx;
    if (is_ctl(m, n))
    {
        if (m[3] == 0x41)
        {
            h->replies++;
            if (h->state == LLK_CANDIDATE) h->state = LLK_LINKED;
        }
        return;
    }
    if (m[0] == 0xF0)
    {
        /* XL LED: F0 00 20 29 02 11 78 tt idx val F7 */
        if (n == 11 && m[1] == 0x00 && m[2] == 0x20 && m[3] == 0x29 && m[5] == 0x11 && m[6] == 0x78
            && m[8] < LLK_XL_LEDS)
            h->b_xl[m[8]] = m[9];
        return;
    }
    if (n == 3)
    {
        const int cell = llk_mini_cell(m[0], m[1]);
        if (cell >= 0) h->b_mini[cell] = (m[0] & 0xF0) == 0x80 ? 0 : m[2];
    }
}

void llk_host_from_b(llk_host_t* h, const uint8_t* bytes, int n)
{
    if (h->state == LLK_NONE) return;
    llk_parse(&h->parser, bytes, n, host_msg, h);
}

static void put3(uint8_t* out, int* outn, uint8_t a, uint8_t b, uint8_t c)
{
    out[(*outn)++] = a; out[(*outn)++] = b; out[(*outn)++] = c;
}

llk_route_t llk_host_mini_in(llk_host_t* h, uint8_t st, uint8_t d1, uint8_t d2, uint8_t* out, int* outn)
{
    if (h->state != LLK_LINKED) return LLK_LOCAL;
    if ((st & 0xF0) == 0xB0 && (d1 == 91 || d1 == 98))
    {
        const uint8_t bit  = d1 == 91 ? 1 : 2;
        const int     down = d2 > 0;
        if (down)
        {
            h->combo_down = (uint8_t)(h->combo_down | bit);
            if (h->combo_down == 3)
            {
                llk_host_set_focus(h, !h->focus_b, out, outn);
                h->combo_swallow = 3;
                return LLK_TOGGLED;
            }
        }
        else
        {
            h->combo_down = (uint8_t)(h->combo_down & ~bit);
            if (h->combo_swallow & bit)
            {
                h->combo_swallow = (uint8_t)(h->combo_swallow & ~bit);
                return LLK_SWALLOWED;
            }
        }
    }
    if (!h->focus_b) return LLK_LOCAL;
    put3(out, outn, st, d1, d2);
    return LLK_FORWARDED;
}

llk_route_t llk_host_xl_in(llk_host_t* h, uint8_t st, uint8_t d1, uint8_t d2, uint8_t* out, int* outn)
{
    if (h->state != LLK_LINKED || !h->focus_b) return LLK_LOCAL;
    put3(out, outn, st, d1, d2);
    return LLK_FORWARDED;
}

void llk_host_set_focus(llk_host_t* h, int focus_b, uint8_t* out, int* outn)
{
    focus_b = focus_b ? 1 : 0;
    if (h->state != LLK_LINKED) focus_b = 0;
    if (focus_b == h->focus_b) return;
    if (h->focus_b) *outn += llk_enc_release(out + *outn); /* B lets go of everything */
    h->focus_b  = focus_b;
    h->pad_sent = 0;
}

void llk_host_presence(llk_host_t* h, uint8_t bits, uint8_t* out, int* outn)
{
    if (h->state != LLK_LINKED || bits == h->presence) return;
    h->presence = bits;
    *outn += llk_enc_presence(out + *outn, bits);
}

void llk_host_pad(llk_host_t* h, uint32_t mask, uint8_t* out, int* outn)
{
    if (h->state != LLK_LINKED || !h->focus_b || mask == h->pad_sent) return;
    h->pad_sent = mask;
    *outn += llk_enc_pad(out + *outn, mask);
}

uint8_t llk_host_mini_led(const llk_host_t* h, int cell, uint8_t own)
{
    if (h->state != LLK_LINKED || cell < 0 || cell >= LLK_MINI_CELLS) return own;
    if (cell == LLK_LOGO) return h->focus_b ? LLK_LOGO_B : LLK_LOGO_A;
    return h->focus_b ? h->b_mini[cell] : own;
}

uint8_t llk_host_xl_led(const llk_host_t* h, int idx, uint8_t own)
{
    if (h->state != LLK_LINKED || !h->focus_b || idx < 0 || idx >= LLK_XL_LEDS) return own;
    return h->b_xl[idx];
}

int llk_host_b_owns(const llk_host_t* h) { return h->state == LLK_LINKED && h->focus_b; }

/* ================================================================== Lab B */

void llk_client_init(llk_client_t* c, const llk_client_cb_t* cb)
{
    memset(c, 0, sizeof *c);
    if (cb) c->cb = *cb;
}

static void client_msg(const uint8_t* m, int n, void* ctx)
{
    llk_client_t* c = (llk_client_t*)ctx;
    c->msgs++;
    if (is_ctl(m, n))
    {
        switch (m[3])
        {
            case 0x01: if (c->cb.hello) c->cb.hello(c->cb.ctx); break;
            case 0x02:
                if (c->pad && c->cb.pad) c->cb.pad(0, c->cb.ctx); /* held pad buttons let go too */
                c->pad = 0;
                if (c->cb.release) c->cb.release(c->cb.ctx);
                break;
            case 0x03:
                if (n >= 6)
                {
                    c->presence = m[4];
                    if (c->cb.presence) c->cb.presence(m[4], c->cb.ctx);
                }
                break;
            case 0x04:
                if (n >= 8)
                {
                    c->pad = (uint32_t)m[4] | ((uint32_t)m[5] << 7) | ((uint32_t)m[6] << 14);
                    if (c->cb.pad) c->cb.pad(c->pad, c->cb.ctx);
                }
                break;
            default: break;
        }
        return;
    }
    if (n != 3) return;
    const uint8_t st = m[0] & 0xF0;
    if (st != 0x80 && st != 0x90 && st != 0xB0) return;
    const uint8_t ch = m[0] & 0x0F;
    if (ch == 0 && c->cb.mini) c->cb.mini(m[0], m[1], m[2], c->cb.ctx);
    else if (ch == 8 && c->cb.xl) c->cb.xl(m[0], m[1], m[2], c->cb.ctx);
}

void llk_client_feed(llk_client_t* c, const uint8_t* bytes, int n) { llk_parse(&c->parser, bytes, n, client_msg, c); }
