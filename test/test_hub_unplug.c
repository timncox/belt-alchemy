/*
 * test_hub_unplug -- src/usbh_hub_midi.c, the real hub driver, compiled on
 * the laptop against a SIMULATED hub (this file) and the ST-shaped headers
 * in fake_usbh/. Nothing here is a capture from hardware: the hub, its ports
 * and the devices answer the requests the way USB 2.0 chapter 11 and the
 * ST host core say they do. What the simulation does check:
 *
 *   - the proven set-up still comes up: Launchpad Mini, Launch Control XL,
 *     XInput pad and the dongle's Realtek Ethernet (rejected), on 4 ports;
 *   - a keyboard pulled out mid-chord: its slot is freed within the port
 *     check period and the chord is released (the glue below is
 *     launchpad.cpp's bind/keys_drop rule: a slot that stops being ready
 *     lets go of what it held); the other devices are not disturbed;
 *   - re-plugged, it enumerates again at a fresh address and plays;
 *   - a quick re-plug between two checks, slots all full, an unplugged
 *     rejected device's port reused, the hub not answering a check;
 *   - the control-pipe rule the ST core needs: the hub driver never
 *     re-points the control pipes, or starts a request, while another
 *     request is in flight; and never talks to a device that has gone.
 */
#include <stdio.h>
#include <string.h>

#include "usb_midi_keys.h"
#include "usbh_hub_midi.h"

static int g_fail = 0, g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        g_checks++;                                                                   \
        if (!(cond)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

/* ================================================================ descriptors */

typedef struct { uint8_t b[512]; uint16_t n; } Desc;

static void put(Desc *d, const uint8_t *p, int n) { memcpy(d->b + d->n, p, (size_t)n); d->n = (uint16_t)(d->n + n); }
static void cfg_begin(Desc *d, uint8_t nif)
{
    const uint8_t c[9] = {9, 0x02, 0, 0, nif, 1, 0, 0x80, 50};
    d->n = 0;
    put(d, c, 9);
}
static void cfg_end(Desc *d) { d->b[2] = (uint8_t)(d->n & 0xFF); d->b[3] = (uint8_t)(d->n >> 8); }
static void itf(Desc *d, uint8_t num, uint8_t neps, uint8_t cls, uint8_t sub, uint8_t proto)
{
    const uint8_t c[9] = {9, 0x04, num, 0, neps, cls, sub, proto, 0};
    put(d, c, 9);
}
static void ep(Desc *d, uint8_t addr, uint8_t attr, uint16_t mps, uint8_t interval)
{
    const uint8_t c[7] = {7, 0x05, addr, attr, (uint8_t)mps, (uint8_t)(mps >> 8), interval};
    put(d, c, 7);
}
static void midi_dev(Desc *d, uint8_t out_ep, uint8_t in_ep)
{
    cfg_begin(d, 2);
    itf(d, 0, 0, 0x01, 0x01, 0x00);
    itf(d, 1, (uint8_t)((out_ep != 0) + (in_ep != 0)), 0x01, 0x03, 0x00);
    if (out_ep) ep(d, out_ep, 0x02, 64, 0);
    if (in_ep)  ep(d, in_ep, 0x02, 64, 0);
    cfg_end(d);
}
static void xinput_dev(Desc *d)
{
    cfg_begin(d, 1);
    itf(d, 0, 2, 0xFF, 0x5D, 0x01);
    ep(d, 0x81, 0x03, 32, 1);
    ep(d, 0x02, 0x03, 32, 8);
    cfg_end(d);
}
static void ethernet_dev(Desc *d)
{
    cfg_begin(d, 1);
    itf(d, 0, 3, 0xFF, 0xFF, 0x00);
    ep(d, 0x81, 0x02, 64, 0); ep(d, 0x02, 0x02, 64, 0); ep(d, 0x83, 0x03, 2, 8);
    cfg_end(d);
}

/* ================================================================ the simulated hub */

#define NPORTS 4

typedef struct
{
    int      present;          /* plugged into its port */
    uint16_t vid, pid;
    Desc     cfg;
    uint8_t  addr;             /* 0 until SET_ADDRESS */
    int      configured;
    uint8_t  pkt[64];          /* what its IN endpoint has to say next */
    int      pkt_len;
    uint32_t in_reads;         /* IN transfers completed with data */
} FDev;

typedef struct
{
    int  connected, enabled, powered;
    int  c_conn, c_reset;
    FDev dev;
} FPort;

typedef struct
{
    int      used, armed;
    uint8_t  ep, addr, type;
    uint8_t *buf;
    uint16_t len;
    uint32_t xfer;
    USBH_URBStateTypeDef urb;
} FPipe;

static FPort    P[NPORTS + 1];       /* 1-based */
static FPipe    pipes[16];
static uint32_t g_now;
static uint8_t  g_ctl_addr = USBH_DEVICE_ADDRESS;
static int      g_violations;        /* control pipes moved / request started mid-request */
static int      g_dead_ctl;          /* a control request to nobody */
static int      g_hub_stall;         /* make the hub refuse GET_STATUS on this port */
static uint32_t g_status_reads;      /* GET_STATUS to the hub */
static uint8_t  g_last_req;
static USBH_HandleTypeDef ph;

uint32_t HAL_GetTick(void) { return g_now; }

static FDev *dev_at(uint8_t addr)
{
    for (int p = 1; p <= NPORTS; p++)
        if (P[p].connected && P[p].enabled && P[p].dev.present && P[p].dev.addr == addr)
            return &P[p].dev;
    return NULL;
}

static USBH_StatusTypeDef hub_request(uint8_t *buf)
{
    const uint8_t  rt = ph.Control.setup.b.bmRequestType, rq = ph.Control.setup.b.bRequest;
    const uint16_t val = ph.Control.setup.b.wValue.w, idx = ph.Control.setup.b.wIndex.w;
    g_last_req = rq;
    if (rq == USB_REQ_GET_DESCRIPTOR && (val >> 8) == 0x29)
    {
        memset(buf, 0, 9);
        buf[0] = 9; buf[1] = 0x29; buf[2] = NPORTS; buf[5] = 10; /* 20 ms power-on */
        return USBH_OK;
    }
    if ((rt & 0x1F) != USB_REQ_RECIPIENT_OTHER || idx < 1 || idx > NPORTS) return USBH_NOT_SUPPORTED;
    FPort *pt = &P[idx];
    if (rq == 0x00) /* GET_STATUS */
    {
        g_status_reads++;
        if (g_hub_stall == idx) return USBH_NOT_SUPPORTED;
        const uint16_t st = (uint16_t)((pt->connected ? 0x0001 : 0) | (pt->enabled ? 0x0002 : 0)
                                       | (pt->powered ? 0x0100 : 0));
        const uint16_t ch = (uint16_t)((pt->c_conn ? 0x0001 : 0) | (pt->c_reset ? 0x0010 : 0));
        buf[0] = (uint8_t)st; buf[1] = (uint8_t)(st >> 8);
        buf[2] = (uint8_t)ch; buf[3] = (uint8_t)(ch >> 8);
        return USBH_OK;
    }
    if (rq == USB_REQ_SET_FEATURE)
    {
        if (val == 8) pt->powered = 1;
        if (val == 4 && pt->connected) /* PORT_RESET: done at once */
        {
            pt->enabled        = 1;
            pt->c_reset        = 1;
            pt->dev.addr       = 0;
            pt->dev.configured = 0;
        }
        return USBH_OK;
    }
    if (rq == USB_REQ_CLEAR_FEATURE)
    {
        if (val == 16) pt->c_conn = 0;
        if (val == 20) pt->c_reset = 0;
        if (val == 1)  pt->enabled = 0;
        return USBH_OK;
    }
    return USBH_NOT_SUPPORTED;
}

static USBH_StatusTypeDef dev_request(uint8_t *buf, uint16_t len)
{
    FDev *d = dev_at(g_ctl_addr);
    if (!d) { g_dead_ctl++; return USBH_FAIL; }
    const uint8_t  rq  = ph.Control.setup.b.bRequest;
    const uint16_t val = ph.Control.setup.b.wValue.w;
    if (rq == USB_REQ_GET_DESCRIPTOR && val == USB_DESC_DEVICE)
    {
        uint8_t dd[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64};
        dd[8] = (uint8_t)d->vid; dd[9] = (uint8_t)(d->vid >> 8);
        dd[10] = (uint8_t)d->pid; dd[11] = (uint8_t)(d->pid >> 8);
        dd[17] = 1;
        memcpy(buf, dd, len < 18 ? len : 18);
        return USBH_OK;
    }
    if (rq == USB_REQ_GET_DESCRIPTOR && val == USB_DESC_CONFIGURATION)
    {
        memcpy(buf, d->cfg.b, len < d->cfg.n ? len : d->cfg.n);
        return USBH_OK;
    }
    if (rq == USB_REQ_SET_ADDRESS) { d->addr = (uint8_t)val; return USBH_OK; }
    if (rq == USB_REQ_SET_CONFIGURATION) { d->configured = 1; return USBH_OK; }
    return USBH_NOT_SUPPORTED;
}

/* As the ST core: CMD_SEND starts the request and answers BUSY; the next
 * call completes it and returns to CMD_SEND. */
USBH_StatusTypeDef USBH_CtlReq(USBH_HandleTypeDef *h, uint8_t *buff, uint16_t length)
{
    if (h->RequestState == CMD_SEND)
    {
        h->Control.buff   = buff;
        h->Control.length = length;
        h->RequestState   = CMD_WAIT;
        return USBH_BUSY;
    }
    if (buff != h->Control.buff) g_violations++; /* a different request mid-flight */
    const USBH_StatusTypeDef st = g_ctl_addr == USBH_DEVICE_ADDRESS ? hub_request(buff)
                                                                     : dev_request(buff, length);
    h->RequestState = CMD_SEND;
    return st;
}

static void setup(uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, uint16_t len)
{
    if (ph.RequestState != CMD_SEND) return;
    ph.Control.setup.b.bmRequestType = rt;
    ph.Control.setup.b.bRequest      = rq;
    ph.Control.setup.b.wValue.w      = val;
    ph.Control.setup.b.wIndex.w      = idx;
    ph.Control.setup.b.wLength.w     = len;
}

USBH_StatusTypeDef USBH_GetDescriptor(USBH_HandleTypeDef *h, uint8_t req_type, uint16_t value_idx,
                                      uint8_t *buff, uint16_t length)
{
    setup((uint8_t)(USB_D2H | req_type), USB_REQ_GET_DESCRIPTOR, value_idx, 0, length);
    return USBH_CtlReq(h, buff, length);
}
USBH_StatusTypeDef USBH_SetAddress(USBH_HandleTypeDef *h, uint8_t a)
{
    setup(USB_H2D, USB_REQ_SET_ADDRESS, a, 0, 0);
    return USBH_CtlReq(h, 0, 0);
}
USBH_StatusTypeDef USBH_SetCfg(USBH_HandleTypeDef *h, uint16_t c)
{
    setup(USB_H2D, USB_REQ_SET_CONFIGURATION, c, 0, 0);
    return USBH_CtlReq(h, 0, 0);
}

USBH_StatusTypeDef USBH_OpenPipe(USBH_HandleTypeDef *h, uint8_t pipe, uint8_t epnum, uint8_t addr,
                                 uint8_t speed, uint8_t type, uint16_t mps)
{
    (void)speed; (void)mps;
    if (pipe == h->Control.pipe_in || pipe == h->Control.pipe_out)
    {
        if (h->RequestState != CMD_SEND) g_violations++;
        g_ctl_addr = addr;
        return USBH_OK;
    }
    if (pipe >= 16) { g_violations++; return USBH_FAIL; }
    pipes[pipe].ep = epnum; pipes[pipe].addr = addr; pipes[pipe].type = type;
    pipes[pipe].armed = 0; pipes[pipe].urb = USBH_URB_IDLE;
    return USBH_OK;
}
USBH_StatusTypeDef USBH_ClosePipe(USBH_HandleTypeDef *h, uint8_t p)
{
    (void)h;
    if (p < 16) { pipes[p].armed = 0; pipes[p].urb = USBH_URB_IDLE; }
    return USBH_OK;
}
uint8_t USBH_AllocPipe(USBH_HandleTypeDef *h, uint8_t ep_addr)
{
    (void)h; (void)ep_addr;
    for (uint8_t i = 2; i < 16; i++)
        if (!pipes[i].used) { memset(&pipes[i], 0, sizeof pipes[i]); pipes[i].used = 1; return i; }
    return 0xFF;
}
USBH_StatusTypeDef USBH_FreePipe(USBH_HandleTypeDef *h, uint8_t i)
{
    (void)h;
    if (i < 16) pipes[i].used = 0;
    return USBH_OK;
}

static USBH_StatusTypeDef rx(uint8_t *b, uint16_t n, uint8_t p)
{
    pipes[p].armed = 1; pipes[p].buf = b; pipes[p].len = n; pipes[p].urb = USBH_URB_IDLE;
    return USBH_OK;
}
USBH_StatusTypeDef USBH_BulkReceiveData(USBH_HandleTypeDef *h, uint8_t *b, uint16_t n, uint8_t p)
{
    (void)h; return rx(b, n, p);
}
USBH_StatusTypeDef USBH_InterruptReceiveData(USBH_HandleTypeDef *h, uint8_t *b, uint8_t n, uint8_t p)
{
    (void)h; return rx(b, n, p);
}
static USBH_StatusTypeDef tx(uint8_t p)
{
    pipes[p].urb = dev_at(pipes[p].addr) ? USBH_URB_DONE : USBH_URB_ERROR;
    return USBH_OK;
}
USBH_StatusTypeDef USBH_BulkSendData(USBH_HandleTypeDef *h, uint8_t *b, uint16_t n, uint8_t p, uint8_t ping)
{
    (void)h; (void)b; (void)n; (void)ping; return tx(p);
}
USBH_StatusTypeDef USBH_InterruptSendData(USBH_HandleTypeDef *h, uint8_t *b, uint8_t n, uint8_t p)
{
    (void)h; (void)b; (void)n; return tx(p);
}

USBH_URBStateTypeDef USBH_LL_GetURBState(USBH_HandleTypeDef *h, uint8_t p)
{
    (void)h;
    FPipe *q = &pipes[p];
    if (!q->armed || !(q->ep & 0x80)) return q->urb;
    FDev *d = dev_at(q->addr);
    if (!d || !d->configured) { q->armed = 0; q->urb = USBH_URB_ERROR; return q->urb; } /* gone: no handshake */
    if (d->pkt_len)
    {
        const int n = d->pkt_len < q->len ? d->pkt_len : q->len;
        memcpy(q->buf, d->pkt, (size_t)n);
        q->xfer = (uint32_t)n; q->armed = 0; q->urb = USBH_URB_DONE;
        d->pkt_len = 0; d->in_reads++;
        return q->urb;
    }
    return USBH_URB_IDLE; /* NAKing: still pending */
}
uint32_t USBH_LL_GetLastXferSize(USBH_HandleTypeDef *h, uint8_t p) { (void)h; return pipes[p].xfer; }
USBH_StatusTypeDef USBH_LL_SetToggle(USBH_HandleTypeDef *h, uint8_t p, uint8_t t)
{
    (void)h; (void)p; (void)t; return USBH_OK;
}

/* ================================================================ the caller (launchpad.cpp's rule) */

/* Per hub slot: a keyboard's held notes; a slot that stops being ready
 * releases them -- bind() + keys_drop() in launchpad.cpp. */
static umk_keys_t g_keys[HUBMIDI_MAX_DEVICES];
static int        g_was_ready[HUBMIDI_MAX_DEVICES];
static umk_msg_t  g_out[256];
static int        g_out_n;
static uint32_t   g_rx_packets[HUBMIDI_MAX_DEVICES];

static void push(const umk_msg_t *m, int n)
{
    for (int i = 0; i < n && g_out_n < 256; i++) g_out[g_out_n++] = m[i];
}

static void on_rx(uint8_t dev, uint8_t *buf, size_t len, void *user)
{
    (void)user;
    g_rx_packets[dev]++;
    if (HUBMIDI_DevKind(dev) != HUBMIDI_KIND_MIDI) return;
    uint16_t vid, pid;
    HUBMIDI_DevId(dev, &vid, &pid);
    if (umk_role(vid, pid, UMK_KIND_MIDI) != UMK_ROLE_KEYS) return;
    umk_msg_t m[16];
    push(m, umk_keys_rx(&g_keys[dev], buf, len, m, 16));
}

static void bind(void)
{
    for (int i = 0; i < HUBMIDI_MAX_DEVICES; i++)
    {
        const int r = HUBMIDI_DevReady(&ph, (uint8_t)i);
        if (g_was_ready[i] && !r)
        {
            umk_msg_t m[16];
            int       n;
            while ((n = umk_keys_release_all(&g_keys[i], m, 16)) > 0) push(m, n);
        }
        if (!g_was_ready[i] && r) umk_keys_reset(&g_keys[i]);
        g_was_ready[i] = r;
    }
}

/* ================================================================ driving it */

static void boot(void)
{
    memset(P, 0, sizeof P);
    memset(pipes, 0, sizeof pipes);
    memset(&ph, 0, sizeof ph);
    memset(g_keys, 0, sizeof g_keys);
    memset(g_was_ready, 0, sizeof g_was_ready);
    memset(g_rx_packets, 0, sizeof g_rx_packets);
    g_out_n = 0; g_now = 1000; g_ctl_addr = USBH_DEVICE_ADDRESS;
    g_violations = g_dead_ctl = g_hub_stall = 0; g_status_reads = 0;
    pipes[0].used = pipes[1].used = 1;
    ph.Control.pipe_out = 0; ph.Control.pipe_in = 1; ph.Control.pipe_size = 64;
    ph.RequestState = CMD_SEND;
    ph.device.speed = 1;
    ph.pActiveClass = USBH_HUB_MIDI_CLASS;
    HUBMIDI_SetReceiveCallback(on_rx, NULL);
    USBH_hub_midi.Init(&ph);
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++)
    {
        g_now++;
        USBH_hub_midi.BgndProcess(&ph);
        bind();
    }
}

static void plug(int port, uint16_t vid, uint16_t pid, const Desc *cfg)
{
    FPort *pt = &P[port];
    memset(&pt->dev, 0, sizeof pt->dev);
    pt->dev.present = 1; pt->dev.vid = vid; pt->dev.pid = pid; pt->dev.cfg = *cfg;
    pt->connected = 1; pt->c_conn = 1; pt->enabled = 0;
}

static void unplug(int port)
{
    FPort *pt = &P[port];
    pt->dev.present = 0; pt->connected = 0; pt->enabled = 0; pt->c_conn = 1;
}

static int slot_of(int port)
{
    const HUBMIDI_Info h = HUBMIDI_GetInfo();
    for (int i = 0; i < HUBMIDI_MAX_DEVICES; i++)
        if (h.dev_port[i] == port) return i;
    return -1;
}

/* Run until the slot on `port` is gone (or max ms); returns ms taken, -1 if never. */
static int run_until_gone(int port, uint32_t max)
{
    for (uint32_t t = 0; t < max; t++)
    {
        if (slot_of(port) < 0) return (int)t;
        run(1);
    }
    return -1;
}

static void send(int port, const uint8_t *pkt, int n)
{
    memcpy(P[port].dev.pkt, pkt, (size_t)n);
    P[port].dev.pkt_len = n;
    run(5);
}

static int out_is(int i, uint8_t a, uint8_t b, uint8_t c)
{
    return i < g_out_n && g_out[i].b[0] == a && g_out[i].b[1] == b && g_out[i].b[2] == c;
}

static Desc d_mini, d_xl, d_pad, d_eth, d_keys, d_keys_in;

/* ================================================================ scenarios */

/* The proven set-up: XL port 1, Mini port 2, pad 3, Ethernet 4. */
static void test_proven_setup(void)
{
    boot();
    plug(1, 0x1235, 0x0061, &d_xl);
    plug(2, 0x1235, 0x0113, &d_mini);
    plug(3, 0x045E, 0x028E, &d_pad);
    plug(4, 0x0BDA, 0x8153, &d_eth);
    run(3000);
    const HUBMIDI_Info h = HUBMIDI_GetInfo();
    CHECK(slot_of(1) >= 0 && slot_of(2) >= 0 && slot_of(3) >= 0);
    CHECK(slot_of(4) < 0);
    CHECK(h.skipped == (1u << 4));
    CHECK(h.done == ((1u << 1) | (1u << 2) | (1u << 3)));
    CHECK(h.dev_kind[slot_of(3)] == HUBMIDI_KIND_XINPUT);
    CHECK(h.unplugs == 0);
    CHECK(h.fail_state == 0 || h.fail_state >= 11); /* only the Ethernet's rejection */
    CHECK(h.checks > 20);       /* the watched ports are being read */
    CHECK(h.check_fails == 0);
    CHECK(g_violations == 0 && g_dead_ctl == 0);
    /* the Mini's pads still arrive while ports are being checked */
    const uint8_t pad[4] = {0x09, 0x90, 11, 127};
    const uint32_t before = g_rx_packets[slot_of(2)];
    for (int i = 0; i < 50; i++) { send(2, pad, 4); run(20); }
    CHECK(g_rx_packets[slot_of(2)] == before + 50);
    /* 2 control + Mini 2 + XL 2 + pad 2 data pipes */
    int used = 0;
    for (int i = 0; i < 16; i++) used += pipes[i].used;
    CHECK(used == 8);
}

/* The bug: a keyboard pulled out of the hub mid-chord. */
static void test_unplug_mid_chord(void)
{
    boot();
    plug(1, 0x1235, 0x0113, &d_mini);
    plug(2, 0x1C75, 0x0001, &d_keys);
    plug(4, 0x0BDA, 0x8153, &d_eth);
    run(3000);
    const int ks = slot_of(2);
    CHECK(ks >= 0);
    const uint8_t chord[12] = {0x09, 0x90, 60, 100, 0x09, 0x90, 64, 100, 0x09, 0x90, 67, 100};
    send(2, chord, 12);
    const uint8_t pedal[4] = {0x0B, 0xB0, 64, 127};
    send(2, pedal, 4);
    CHECK(g_out_n == 4 && umk_keys_held_count(&g_keys[ks]) == 3);
    const uint8_t old_addr = P[2].dev.addr;

    g_out_n = 0;
    const int pipes_before = (int)pipes[2].used + pipes[3].used + pipes[4].used + pipes[5].used
                             + pipes[6].used + pipes[7].used;
    unplug(2);
    const int t = run_until_gone(2, 2000);
    printf("  keyboard unplugged mid-chord: slot freed after %d ms\n", t);
    CHECK(t >= 0 && t <= 300); /* two watched ports (1, 2) and Ethernet 4: <= 3 x 100 ms */
    run(2);
    CHECK(g_out_n == 4);
    CHECK(out_is(0, 0x80, 60, 0) && out_is(1, 0x80, 64, 0) && out_is(2, 0x80, 67, 0));
    CHECK(out_is(3, 0xB0, 64, 0));
    CHECK(HUBMIDI_GetInfo().unplugs == 1);
    const int pipes_after = (int)pipes[2].used + pipes[3].used + pipes[4].used + pipes[5].used
                            + pipes[6].used + pipes[7].used;
    CHECK(pipes_after == pipes_before - 2); /* its two pipes given back */
    CHECK(slot_of(1) >= 0);                 /* the Mini is still there */
    CHECK(g_dead_ctl == 0 && g_violations == 0);

    /* nothing more comes out of it */
    g_out_n = 0;
    run(2000);
    CHECK(g_out_n == 0);
    CHECK(HUBMIDI_GetInfo().unplugs == 1);

    /* plugged back in: enumerated afresh, plays */
    plug(2, 0x1C75, 0x0001, &d_keys);
    run(1500);
    CHECK(slot_of(2) >= 0);
    CHECK(P[2].dev.configured && P[2].dev.addr != 0);
    printf("  re-plugged: address %u (was %u)\n", P[2].dev.addr, old_addr);
    const uint8_t note[4] = {0x09, 0x90, 72, 90};
    g_out_n = 0;
    send(2, note, 4);
    CHECK(g_out_n == 1 && out_is(0, 0x90, 72, 90));
    CHECK(g_dead_ctl == 0 && g_violations == 0);
}

/* Out and back in between two checks: connection-change says it is a new
 * device; the old slot is freed and the new one enumerated. */
static void test_quick_replug(void)
{
    boot();
    plug(3, 0x0582, 0x0001, &d_keys);
    run(2000);
    const int s = slot_of(3);
    CHECK(s >= 0);
    const uint8_t on[4] = {0x09, 0x90, 50, 100};
    send(3, on, 4);
    g_out_n = 0;
    unplug(3);
    plug(3, 0x0582, 0x0001, &d_keys); /* back before any check ran */
    run(1500);
    CHECK(HUBMIDI_GetInfo().unplugs == 1);
    CHECK(out_is(0, 0x80, 50, 0));
    CHECK(slot_of(3) >= 0 && P[3].dev.configured);
    CHECK(g_dead_ctl == 0 && g_violations == 0);
}

/* All four slots full (S_FULL): an unplug is still seen, and the freed slot
 * takes the next device. */
static void test_full_then_unplug(void)
{
    boot();
    plug(1, 0x1235, 0x0113, &d_mini);
    plug(2, 0x1235, 0x0061, &d_xl);
    plug(3, 0x045E, 0x028E, &d_pad);
    plug(4, 0x0582, 0x0002, &d_keys_in); /* a keyboard with no OUT endpoint */
    run(3000);
    CHECK(slot_of(1) >= 0 && slot_of(2) >= 0 && slot_of(3) >= 0 && slot_of(4) >= 0);
    CHECK(HUBMIDI_GetInfo().state == 22); /* S_FULL */
    unplug(3);
    const int t = run_until_gone(3, 2000);
    CHECK(t >= 0 && t <= 400);
    run(700);
    CHECK(HUBMIDI_GetInfo().state != 22);
    plug(3, 0x0582, 0x0003, &d_keys);
    run(1500);
    CHECK(slot_of(3) >= 0);
    CHECK(g_dead_ctl == 0 && g_violations == 0);
}

/* The Ethernet chip's port, rejected, then its device unplugged: the port
 * is scanned again, and a keyboard plugged there is taken. */
static void test_rejected_port_reused(void)
{
    boot();
    plug(4, 0x0BDA, 0x8153, &d_eth);
    run(2000);
    CHECK(HUBMIDI_GetInfo().skipped == (1u << 4));
    const uint16_t rej = HUBMIDI_GetInfo().skipped;
    (void)rej;
    run(2000);
    CHECK(HUBMIDI_GetInfo().skipped == (1u << 4)); /* stays rejected while it is there */
    unplug(4);
    run(500);
    CHECK(HUBMIDI_GetInfo().skipped == 0);
    plug(4, 0x0582, 0x0001, &d_keys);
    run(1500);
    CHECK(slot_of(4) >= 0);
    CHECK(g_dead_ctl == 0 && g_violations == 0);
}

/* The hub refuses the port-status read of a watched port for a while:
 * never fatal, nothing released, and it carries on once the hub answers.
 * (A refusal while SCANNING a free port still fails the hub, as before --
 * unchanged on purpose.) */
static void test_hub_refuses_check(void)
{
    boot();
    plug(2, 0x0582, 0x0001, &d_keys);
    run(2000);
    const uint8_t on[4] = {0x09, 0x90, 55, 100};
    send(2, on, 4);
    g_out_n = 0;
    g_hub_stall = 2;
    run(1000);
    CHECK(HUBMIDI_GetInfo().check_fails > 0);
    CHECK(HUBMIDI_GetInfo().fail_state == 0);
    CHECK(slot_of(2) >= 0 && g_out_n == 0);
    g_hub_stall = 0;
    unplug(2);
    CHECK(run_until_gone(2, 1000) >= 0);
    run(2);
    CHECK(out_is(0, 0x80, 55, 0));
}

/* Unplug and re-plug a hundred times: addresses wrap, pipes do not leak. */
static void test_many_replugs(void)
{
    boot();
    plug(1, 0x1235, 0x0113, &d_mini);
    run(2000);
    for (int i = 0; i < 100; i++)
    {
        plug(2, 0x0582, 0x0001, &d_keys);
        run(800);
        if (slot_of(2) < 0) break;
        unplug(2);
        run(400);
    }
    plug(2, 0x0582, 0x0001, &d_keys);
    run(800);
    CHECK(slot_of(2) >= 0 && slot_of(1) >= 0);
    CHECK(HUBMIDI_GetInfo().unplugs == 100);
    CHECK(P[2].dev.addr >= 2 && P[2].dev.addr <= 127 && P[2].dev.addr != P[1].dev.addr);
    int used = 0;
    for (int i = 0; i < 16; i++) used += pipes[i].used;
    CHECK(used == 6); /* 2 control + Mini 2 + keyboard 2 */
    CHECK(g_dead_ctl == 0 && g_violations == 0);
}

int main(void)
{
    midi_dev(&d_mini, 0x01, 0x81);
    midi_dev(&d_xl, 0x02, 0x81);
    midi_dev(&d_keys, 0x02, 0x81);
    midi_dev(&d_keys_in, 0, 0x81);
    xinput_dev(&d_pad);
    ethernet_dev(&d_eth);

    test_proven_setup();
    test_unplug_mid_chord();
    test_quick_replug();
    test_full_then_unplug();
    test_rejected_port_reused();
    test_hub_refuses_check();
    test_many_replugs();
    printf("test_hub_unplug: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
