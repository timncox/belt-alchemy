/* launchpad -- see launchpad.h. */
#include "launchpad.h"

#include <stdio.h>
#include <string.h>

#include "daisy_core.h"
#include "hid/usb_host.h"
#include "usbh_midi.h"
#include "usbh_hub_midi.h"
#include "usb_midi_keys.h"
#include "hid/usb_midi.h"
#include "lab_link.h"

extern "C" USBH_HandleTypeDef hUsbHostHS; /* libDaisy's host handle */

namespace xl { void Flush_(); }
namespace keys { void Push_(const umk_msg_t* m, int n); }
namespace lp { void release_all_(); void redraw_(); }

/* ============================================================== transport */
/*
 * A "link" is one USB-MIDI device the host can talk to: the device plugged
 * straight in (libDaisy's MIDI class), or one of the hub driver's slots.
 * Devices are recognised by vendor/product id and bound to a role
 * (umk_role): the Mini and the XL by id, any other MIDI device is a keyboard.
 */
namespace
{

constexpr uint16_t kNovation = 0x1235;

constexpr int kDirect   = 0;                         /* link id: no hub */
constexpr int kHub0     = 1;                         /* link id: hub slot 0 */
constexpr int kLinks    = 1 + HUBMIDI_MAX_DEVICES;
constexpr int kVirtual  = kLinks;                    /* Lab B: a controller on Lab A */
constexpr int kNone     = -1;

/* Another Alchemy Lab in Link mode (lab_link.h). libDaisy's device-side MIDI
 * keeps the stock ST ids, so the id only makes it a candidate: it counts as
 * a Lab once it answers the hello. */
constexpr uint16_t kLabVid = 0x0483, kLabPid = 0x5740;

daisy::USBHostHandle g_usbh;
bool                 g_direct_ready = false; /* MIDI class active, pipes open */
bool                 g_failed       = false; /* abort, unsupported or error seen */
int                  g_mini_link    = kNone;
int                  g_xl_link      = kNone;
uint8_t              g_keys_links   = 0;     /* bit n = link n is a keyboard */
umk_keys_t           g_keys[kLinks];         /* per keyboard: notes held */
volatile uint32_t    g_pad_buttons  = 0;
uint32_t             g_pad_reports  = 0;
uint8_t              g_pad_last[6];         /* the last report's head, any type */

uint32_t g_rx_count = 0, g_tx_count = 0;

/* Lab A side: the linked Lab B and what is queued for it. */
llk_host_t g_lab;
int        g_lab_link  = kNone;
uint8_t    g_lab_q[512];
int        g_lab_qn    = 0;
uint32_t   g_lab_hello = 0;   /* ms of the last hello */
llk_state_t g_lab_was  = LLK_NONE;

void lab_queue(const uint8_t* b, int n)
{
    if (g_lab_qn + n > (int)sizeof g_lab_q) return; /* full: drop, never block */
    memcpy(g_lab_q + g_lab_qn, b, (size_t)n);
    g_lab_qn += n;
}

/* Lab B side ("Link" mode): libDaisy's USB-MIDI device on the front port. */
bool                     g_client = false;
daisy::MidiUsbTransport  g_dev;
llk_client_t             g_cl;
uint8_t                  g_cl_presence = 0;
uint8_t                  g_cl_out[512];
int                      g_cl_outn     = 0;
uint32_t                 g_cl_rx       = 0;
/* Bytes arrive in the USB interrupt: a single-producer ring, drained by Poll. */
uint8_t                  g_cl_ring[2048];
volatile uint32_t        g_cl_head = 0, g_cl_tail = 0;

bool via_hub() { return g_usbh.IsActiveClass(USBH_HUB_MIDI_CLASS); }

bool link_ready(int link)
{
    if (link == kVirtual) return g_client;
    if (link == kDirect) return g_direct_ready && g_usbh.IsActiveClass(USBH_MIDI_CLASS);
    return HUBMIDI_DevReady(&hUsbHostHS, (uint8_t)(link - kHub0));
}

void link_id(int link, uint16_t* vid, uint16_t* pid)
{
    if (link == kDirect)
    {
        *vid = hUsbHostHS.device.DevDesc.idVendor;
        *pid = hUsbHostHS.device.DevDesc.idProduct;
    }
    else
        HUBMIDI_DevId((uint8_t)(link - kHub0), vid, pid);
}

uint16_t link_out_size(int link)
{
    if (link == kVirtual) return 0; /* Lab B sends its LEDs through client_flush */
    if (link == kDirect)
    {
        if (!hUsbHostHS.pActiveClass || !hUsbHostHS.pActiveClass->pData) return 0;
        return ((const MIDI_HandleTypeDef*)hUsbHostHS.pActiveClass->pData)->OutEpSize;
    }
    return HUBMIDI_OutSize(&hUsbHostHS, (uint8_t)(link - kHub0));
}

/* Non-blocking; false = busy or gone, nothing was sent. */
bool link_send(int link, uint8_t* buf, uint16_t len)
{
    const bool ok = (link == kDirect)
                        ? USBH_MIDI_Transmit(&hUsbHostHS, buf, len) == MIDI_OK
                        : HUBMIDI_Transmit(&hUsbHostHS, (uint8_t)(link - kHub0), buf, len)
                              == USBH_OK;
    if (ok) g_tx_count++;
    return ok;
}

/* Each device has its own transmit buffer: two transfers may be in flight. */
uint8_t DMA_BUFFER_MEM_SECTION g_tx_mini[64];
uint8_t DMA_BUFFER_MEM_SECTION g_tx_xl[64];
uint8_t DMA_BUFFER_MEM_SECTION g_tx_lab[64];

void mini_rx(uint8_t* buf, size_t len);
void xl_rx(uint8_t* buf, size_t len);
void mini_bound();
void xl_bound();
void mini_dropped();
void xl_dropped();
void client_poll();
void xl_redraw();
void cl_isr(uint8_t* data, size_t size, void*);
void cl_mini(uint8_t st, uint8_t d1, uint8_t d2, void*);
void cl_xl(uint8_t st, uint8_t d1, uint8_t d2, void*);
void cl_hello(void*);
void cl_release(void*);
void cl_presence(uint8_t bits, void*);
void cl_pad(uint32_t mask, void*);

/* XInput input report: type 0x00, length 0x14, wButtons, LT, RT, sticks. */
void pad_rx(const uint8_t* buf, size_t len)
{
    for (size_t i = 0; i < 6 && i < len; i++) g_pad_last[i] = buf[i];
    if (len < 6 || buf[0] != 0x00) return; /* 0x08 etc: status, not input */
    uint32_t b = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8);
    if (buf[4] > 64) b |= 1u << 16;
    if (buf[5] > 64) b |= 1u << 17;
    g_pad_buttons = b;
    g_pad_reports++;
    if (llk_host_b_owns(&g_lab))
    {
        uint8_t m[8];
        int     n = 0;
        llk_host_pad(&g_lab, b, m, &n);
        lab_queue(m, n);
    }
}

bool is_pad(int link)
{
    return link != kDirect && HUBMIDI_DevKind((uint8_t)(link - kHub0)) == HUBMIDI_KIND_XINPUT;
}

bool is_keys(int link) { return link < kLinks && ((g_keys_links >> link) & 1u); }

void keys_rx(int link, const uint8_t* buf, size_t len)
{
    umk_msg_t m[16];
    keys::Push_(m, umk_keys_rx(&g_keys[link], buf, len, m, 16));
}

/* The keyboard went away: let go of what it held. */
void keys_drop(int link)
{
    umk_msg_t m[16];
    int       n;
    while ((n = umk_keys_release_all(&g_keys[link], m, 16)) > 0) keys::Push_(m, n);
    g_keys_links = (uint8_t)(g_keys_links & ~(1u << link));
}

void dispatch(int link, uint8_t* buf, size_t len)
{
    if (link == g_lab_link)
    {
        uint8_t b[256];
        llk_host_from_b(&g_lab, b, llk_from_usb(buf, (int)len, b, sizeof b));
        return;
    }
    if (is_pad(link)) { pad_rx(buf, len); return; }
    g_rx_count++;
    if (is_keys(link)) keys_rx(link, buf, len);
    else if (link == g_mini_link) mini_rx(buf, len);
    else if (link == g_xl_link) xl_rx(buf, len);
}

void on_rx_direct(uint8_t* buf, size_t len, void*) { dispatch(kDirect, buf, len); }
void on_rx_hub(uint8_t dev, uint8_t* buf, size_t len, void*)
{
    dispatch(kHub0 + dev, buf, len);
}

void on_class_active(void*)
{
    if (!g_usbh.IsActiveClass(USBH_MIDI_CLASS)) return;
    USBH_MIDI_SetReceiveCallback(&hUsbHostHS, on_rx_direct, nullptr);
    g_direct_ready = true;
}

void on_error(void*) { g_failed = true; }

void on_disconnect(void*)
{
    g_direct_ready = false;
    if (g_mini_link != kNone) mini_dropped();
    if (g_xl_link != kNone) xl_dropped();
    g_mini_link    = kNone;
    g_xl_link      = kNone;
    g_pad_buttons  = 0;
    for (int l = 0; l < kLinks; l++)
        if (is_keys(l)) keys_drop(l);
    if (g_lab_link != kNone) { llk_host_detach(&g_lab); g_lab_link = kNone; g_lab_qn = 0; }
}

/* Bind ready links to roles by id; drop roles whose link went away (the
 * hub driver frees a slot when its device is unplugged): whatever that
 * device was holding down is released, as if let go. */
void bind()
{
    if (g_mini_link != kNone && !link_ready(g_mini_link)) { mini_dropped(); g_mini_link = kNone; }
    if (g_xl_link != kNone && !link_ready(g_xl_link)) { xl_dropped(); g_xl_link = kNone; }
    for (int l = 0; l < kLinks; l++)
        if (is_keys(l) && !link_ready(l)) keys_drop(l);
    if (g_lab_link != kNone && !link_ready(g_lab_link))
    {
        const bool had = llk_host_b_owns(&g_lab);
        llk_host_detach(&g_lab);
        g_lab_link = kNone;
        g_lab_qn   = 0;
        if (had) lp::redraw_(); /* Lab A's own LEDs come back */
    }
    bool pad = false;
    for (int l = kHub0; l < kLinks; l++)
        if (link_ready(l) && is_pad(l)) pad = true;
    if (!pad) g_pad_buttons = 0;
    for (int l = 0; l < kLinks; l++)
    {
        if (l == g_mini_link || l == g_xl_link || l == g_lab_link || is_keys(l) || !link_ready(l)
            || is_pad(l))
            continue;
        uint16_t vid = 0, pid = 0;
        link_id(l, &vid, &pid);
        if (vid == kLabVid && pid == kLabPid && g_lab_link == kNone && l != kDirect)
        {
            g_lab_link  = l;
            g_lab_qn    = 0;
            g_lab_hello = 0;
            llk_host_attach(&g_lab);
            continue;
        }
        const int role = umk_role(vid, pid, UMK_KIND_MIDI);
        if (role == UMK_ROLE_MINI && g_mini_link == kNone) { g_mini_link = l; mini_bound(); }
        else if (role == UMK_ROLE_XL && g_xl_link == kNone) { g_xl_link = l; xl_bound(); }
        else if (role == UMK_ROLE_KEYS)
        {
            umk_keys_reset(&g_keys[l]);
            g_keys_links = (uint8_t)(g_keys_links | (1u << l));
        }
    }
}

void lab_poll(uint32_t now)
{
    if (g_lab_link == kNone) return;
    uint8_t m[16];
    int     n = 0;
    if (g_lab.state == LLK_CANDIDATE && (g_lab_hello == 0 || now - g_lab_hello >= 500))
    {
        g_lab_hello = now ? now : 1;
        n = llk_enc_hello(m);
        lab_queue(m, n);
        g_lab.hellos++;
    }
    if (g_lab.state == LLK_LINKED)
    {
        bool pad = false;
        for (int l = kHub0; l < kLinks; l++)
            if (link_ready(l) && is_pad(l)) pad = true;
        const uint8_t bits = (uint8_t)((g_mini_link != kNone ? LLK_HAS_MINI : 0)
                                       | (g_xl_link != kNone ? LLK_HAS_XL : 0) | (pad ? LLK_HAS_PAD : 0));
        n = 0;
        llk_host_presence(&g_lab, bits, m, &n);
        lab_queue(m, n);
    }
    if (g_lab.state != g_lab_was)
    {
        g_lab_was = g_lab.state;
        lp::redraw_(); /* the logo shows whose controllers they are */
    }
    if (g_lab_qn == 0) return;
    uint16_t ep = link_out_size(g_lab_link);
    if (ep < 4) return;
    if (ep > sizeof g_tx_lab) ep = sizeof g_tx_lab;
    int       used = 0;
    const int len  = llk_to_usb(g_lab_q, g_lab_qn, g_tx_lab, ep, &used);
    if (len == 0 || !link_send(g_lab_link, g_tx_lab, (uint16_t)len)) return; /* retry next poll */
    memmove(g_lab_q, g_lab_q + used, (size_t)(g_lab_qn - used));
    g_lab_qn -= used;
}

/* Focus moved between the Labs: whoever lost it lets go of what it held
 * (Lab B is told by lab_link), and every LED is drawn again from the new
 * owner's picture. */
void focus_changed()
{
    if (llk_host_b_owns(&g_lab))
    {
        lp::release_all_();
        uint8_t m[8];
        lab_queue(m, llk_enc_hello(m)); /* B resends every LED (sends are unacknowledged) */
    }
    lp::redraw_();
}

/* ---------------------------------------------------------------- USB-MIDI */

/* SysEx bytes as USB-MIDI packets on one cable; returns packets written. */
int sysex_packets(const uint8_t* sx, int n, uint8_t cable, uint8_t* out)
{
    int k = 0, i = 0;
    while (i < n)
    {
        const int rest = n - i;
        uint8_t*  p    = &out[4 * k++];
        if (rest > 3)
        {
            p[0] = (uint8_t)((cable << 4) | 0x4);
            p[1] = sx[i]; p[2] = sx[i + 1]; p[3] = sx[i + 2];
            i += 3;
        }
        else
        {
            p[0] = (uint8_t)((cable << 4) | (rest == 1 ? 0x5 : rest == 2 ? 0x6 : 0x7));
            p[1] = sx[i]; p[2] = rest > 1 ? sx[i + 1] : 0; p[3] = rest > 2 ? sx[i + 2] : 0;
            i += rest;
        }
    }
    return k;
}

} // namespace

/* ============================================================== Launchpad Mini MK3 */

namespace lp
{

namespace
{

constexpr int kGrid  = 64;
constexpr int kSide0 = 64;
constexpr int kTop0  = 72;
constexpr int kLogo  = 80;
constexpr int kCells = 81;
constexpr uint8_t kUnknown = 0xFF; /* not yet sent since connect */

bool g_need_init = false; /* send Programmer mode next */
int  g_rx_cable  = -1;    /* cable events are taken from */

uint8_t g_want[kCells];
uint8_t g_sent[kCells];
int     g_scan = 0; /* where the next diff scan starts, for fairness */

constexpr int kQueue = 64;
Event         g_q[kQueue];
volatile int  g_qhead = 0, g_qtail = 0;

Diag    g_diag    = {0, 0, false, 0};
uint8_t g_last_gs = 0;
uint8_t g_last_es = 0;

/* Transition trace: (ms, host state, enum state). */
struct Tr { uint32_t t; uint8_t gs, es; };
constexpr int kTrace = 64;
Tr      g_tr[kTrace];
int     g_tr_n = 0;

void push(Kind k, uint8_t x, uint8_t y, bool down)
{
    int next = (g_qhead + 1) % kQueue;
    if (next == g_qtail) return; /* full: drop, never block */
    g_q[g_qhead] = Event{k, x, y, down};
    g_qhead      = next;
}

/* Which pads and buttons are down now (cell index as g_want), so an unplug
 * can release them. */
uint8_t g_down[(kLogo + 7) / 8];

void press(Kind k, uint8_t x, uint8_t y, bool down)
{
    const int i = k == Kind::Grid ? y * 8 + x : k == Kind::Side ? kSide0 + y : kTop0 + x;
    if (down) g_down[i >> 3] = (uint8_t)(g_down[i >> 3] | (1u << (i & 7)));
    else      g_down[i >> 3] = (uint8_t)(g_down[i >> 3] & ~(1u << (i & 7)));
    push(k, x, y, down);
}

void release_held()
{
    for (int i = 0; i < kLogo; i++)
    {
        if (!(g_down[i >> 3] & (1u << (i & 7)))) continue;
        if (i < kGrid)      push(Kind::Grid, (uint8_t)(i % 8), (uint8_t)(i / 8), false);
        else if (i < kTop0) push(Kind::Side, 0, (uint8_t)(i - kSide0), false);
        else                push(Kind::Top, (uint8_t)(i - kTop0), 0, false);
    }
    memset(g_down, 0, sizeof g_down);
}

/* One LED as a USB-MIDI packet on the given cable. */
void cell_packet(int i, uint8_t col, uint8_t cable, uint8_t* p)
{
    uint8_t note;
    bool    cc;
    if (i < kGrid)        { note = (uint8_t)(10 * (8 - i / 8) + (i % 8) + 1); cc = false; }
    else if (i < kTop0)   { note = (uint8_t)(10 * (8 - (i - kSide0)) + 9);     cc = true;  }
    else if (i < kLogo)   { note = (uint8_t)(91 + (i - kTop0));                cc = true;  }
    else                  { note = 99;                                          cc = true;  }
    p[0] = (uint8_t)((cable << 4) | (cc ? 0x0B : 0x09));
    p[1] = cc ? 0xB0 : 0x90;
    p[2] = note;
    p[3] = col;
}

void flush()
{
    const int link = g_mini_link;
    if (link == kNone) return;
    uint16_t ep = link_out_size(link);
    if (ep < 4) return;
    if (ep > sizeof g_tx_mini) ep = sizeof g_tx_mini;
    const int max_pk = ep / 4;
    int       n      = 0;

    int  sent_idx[16];
    int  sent_n = 0;
    bool init   = g_need_init;
    if (init)
    {
        /* Programmer mode, F0 00 20 29 02 0D 0E 01 F7, on both cables. */
        static const uint8_t sx[9] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x01, 0xF7};
        n  = sysex_packets(sx, 9, 0, g_tx_mini);
        n += sysex_packets(sx, 9, 1, g_tx_mini + 4 * n);
    }
    else
    {
        /* Each changed LED goes out on both cables: two packets. */
        for (int k = 0; k < kCells && n + 2 <= max_pk && sent_n < 16; k++)
        {
            const int     i    = (g_scan + k) % kCells;
            const uint8_t want = llk_host_mini_led(&g_lab, i, g_want[i]);
            if (want == g_sent[i]) continue;
            cell_packet(i, want, 0, &g_tx_mini[4 * n]);
            cell_packet(i, want, 1, &g_tx_mini[4 * (n + 1)]);
            sent_idx[sent_n++] = i;
            n += 2;
        }
        if (n == 0) return;
    }

    if (!link_send(link, g_tx_mini, (uint16_t)(4 * n))) return; /* retry next poll */

    if (init)
    {
        g_need_init = false;
        for (int i = 0; i < kCells; i++) g_sent[i] = kUnknown; /* send every LED */
    }
    else
    {
        for (int j = 0; j < sent_n; j++)
            g_sent[sent_idx[j]] = llk_host_mini_led(&g_lab, sent_idx[j], g_want[sent_idx[j]]);
        g_scan = (sent_idx[sent_n - 1] + 1) % kCells;
    }
}

} // namespace

void Init()
{
    for (int i = 0; i < kCells; i++) g_want[i] = g_sent[i] = kOff;
    daisy::USBHostHandle::Config cfg;
    cfg.disconnect_callback   = on_disconnect;
    cfg.class_active_callback = on_class_active;
    cfg.error_callback        = on_error;
    g_usbh.Init(cfg);
    g_usbh.RegisterClass(USBH_MIDI_CLASS);
    g_usbh.RegisterClass(USBH_HUB_MIDI_CLASS);   /* a powered adapter with a hub */
    HUBMIDI_SetReceiveCallback(on_rx_hub, nullptr);
}

void Poll(uint32_t now)
{
    if (g_client) { client_poll(); return; }
    g_usbh.Process();
    /* libDaisy reads the whole configuration descriptor into a 256-byte
     * buffer, however long the device says it is (a big keyboard's can be
     * longer): ask for no more than fits. The request goes out on the next
     * Process, so this is in time. */
    if (hUsbHostHS.gState == HOST_ENUMERATION && hUsbHostHS.EnumState == ENUM_GET_FULL_CFG_DESC
        && hUsbHostHS.RequestState == CMD_SEND
        && hUsbHostHS.device.CfgDesc.wTotalLength > USBH_MAX_SIZE_CONFIGURATION)
        hUsbHostHS.device.CfgDesc.wTotalLength = USBH_MAX_SIZE_CONFIGURATION;
    const uint8_t gs = (uint8_t)hUsbHostHS.gState;
    const uint8_t es = (uint8_t)hUsbHostHS.EnumState;
    if ((gs != g_last_gs || es != g_last_es) && g_tr_n < kTrace)
        g_tr[g_tr_n++] = Tr{now, gs, es};
    g_last_es = es;
    if (gs == HOST_ABORT_STATE && g_last_gs != HOST_ABORT_STATE)
    {
        if (g_diag.aborts == 0)
        {
            g_diag.state_before_abort = g_last_gs;
            g_diag.enum_at_abort      = (uint8_t)(hUsbHostHS.EnumState + 1);
        }
        if (g_diag.aborts < 0xFFFF) g_diag.aborts++;
    }
    g_last_gs = gs;
    if (hUsbHostHS.device.DevDesc.idVendor == kNovation) g_diag.vid_ok = true;

    bind();
    lab_poll(now);
    flush();
    xl::Flush_();
}

/* ------------------------------------------------------------ Link (Lab B) */

void InitLink()
{
    for (int i = 0; i < kCells; i++) g_want[i] = g_sent[i] = kOff;
    g_client = true;
    const llk_client_cb_t cb = {cl_mini, cl_xl, cl_hello, cl_release, cl_presence, cl_pad, nullptr};
    llk_client_init(&g_cl, &cb);
    daisy::MidiUsbTransport::Config cfg;
    cfg.periph         = daisy::MidiUsbTransport::Config::EXTERNAL;
    cfg.tx_retry_count = 1;
    g_dev.Init(cfg);
    g_dev.StartRx(cl_isr, nullptr);
}

bool IsLinkClient() { return g_client; }
uint8_t LinkState() { return g_client ? (uint8_t)(g_cl_presence ? LLK_LINKED : LLK_NONE) : (uint8_t)g_lab.state; }
bool LinkFocusB() { return llk_host_b_owns(&g_lab); }

void SetLinkFocus(bool b)
{
    if (g_client || g_lab.state != LLK_LINKED || b == (bool)g_lab.focus_b) return;
    uint8_t m[16];
    int     n = 0;
    llk_host_set_focus(&g_lab, b ? 1 : 0, m, &n);
    lab_queue(m, n);
    focus_changed();
}

/* Up to 16 USB-MIDI packets of LED changes per call (a Mini cell is one, an
 * XL LED four); returns bytes written. Marks them sent: Lab A's hello (at
 * link-up and at every focus switch to B) asks for everything again. */
int client_leds_(uint8_t* out, int budget)
{
    int n = 0;
    if (g_need_init) { g_need_init = false; for (int i = 0; i < kCells; i++) g_sent[i] = kUnknown; }
    for (int k = 0; k < kCells && budget > 0; k++)
    {
        const int i = (g_scan + k) % kCells;
        if (g_want[i] == g_sent[i]) continue;
        n += llk_enc_mini_led(out + n, i, g_want[i]);
        g_sent[i] = g_want[i];
        g_scan    = (i + 1) % kCells;
        budget--;
    }
    return n;
}

bool Connected() { return g_mini_link != kNone; }

void release_all_()
{
    release_held();
    xl_dropped();
    g_pad_buttons = 0;
}

void redraw_()
{
    for (int i = 0; i < kCells; i++) g_sent[i] = kUnknown;
    xl_redraw();
}

uint8_t Stage()
{
    if (g_client) return g_cl_presence ? 3 : g_cl_rx ? 1 : 0;
    if (g_mini_link != kNone || g_xl_link != kNone || g_keys_links) return 3;
    for (int l = kHub0; l < kLinks; l++)
        if (link_ready(l) && is_pad(l)) return 3;
    if (hUsbHostHS.gState == HOST_ABORT_STATE) g_failed = true;
    if (via_hub())
        return HUBMIDI_GetInfo().fail_state && !HUBMIDI_Ready(&hUsbHostHS) ? 2 : 1;
    if (g_failed) return 2;
    return hUsbHostHS.device.is_connected ? 1 : 0;
}

bool PopEvent(Event* e)
{
    if (g_qtail == g_qhead) return false;
    *e      = g_q[g_qtail];
    g_qtail = (g_qtail + 1) % kQueue;
    return true;
}

void SetGrid(uint8_t x, uint8_t y, uint8_t col)
{
    if (x < 8 && y < 8) g_want[y * 8 + x] = col;
}
void SetSide(uint8_t y, uint8_t col)
{
    if (y < 8) g_want[kSide0 + y] = col;
}
void SetTop(uint8_t x, uint8_t col)
{
    if (x < 8) g_want[kTop0 + x] = col;
}
void SetLogo(uint8_t col) { g_want[kLogo] = col; }

void ClearAll()
{
    for (int i = 0; i < kCells; i++) g_want[i] = kOff;
}

Diag Diagnostics() { return g_diag; }

int Report(char* b, int cap)
{
    int n = 0;
#define OUT(...) do { if (n < cap) n += snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); } while (0)
    OUT("launchpad host report\n");
    OUT("aborts %u, state before first abort %u, enum at first abort %u, vid_ok %d\n",
        (unsigned)g_diag.aborts, (unsigned)g_diag.state_before_abort,
        (unsigned)g_diag.enum_at_abort, (int)g_diag.vid_ok);
    OUT("rx %lu tx %lu, mini link %d, xl link %d, direct %d\n", (unsigned long)g_rx_count,
        (unsigned long)g_tx_count, g_mini_link, g_xl_link, (int)g_direct_ready);
    {
        const HUBMIDI_Info h = HUBMIDI_GetInfo();
        OUT("hub: active %d state %u ports %u port %u status %04x last fail state %u code %u "
            "skipped %04x done %04x, port checks %lu (no answer %lu), unplugs %u\n",
            (int)via_hub(), h.state, h.ports, h.port, h.port_status, h.fail_state, h.fail_code,
            h.skipped, h.done, (unsigned long)h.checks, (unsigned long)h.check_fails,
            (unsigned)h.unplugs);
        for (int d = 0; d < HUBMIDI_MAX_DEVICES; d++)
            OUT("  slot %d: port %u %04x:%04x kind %u rx armed %lu data %lu nak %lu err %lu stale %lu\n", d,
                h.dev_port[d], h.dev_vid[d], h.dev_pid[d], h.dev_kind[d],
                (unsigned long)h.rx_arms[d], (unsigned long)h.rx_done[d],
                (unsigned long)h.rx_nak[d], (unsigned long)h.rx_err[d],
                (unsigned long)h.rx_stale[d]);
    }
    OUT("pad: reports %lu, held %05lx, last %02x %02x %02x %02x %02x %02x\n",
        (unsigned long)g_pad_reports, (unsigned long)g_pad_buttons, g_pad_last[0], g_pad_last[1],
        g_pad_last[2], g_pad_last[3], g_pad_last[4], g_pad_last[5]);
    OUT("keys: links %02x", g_keys_links);
    for (int l = 0; l < kLinks; l++)
    {
        if (!is_keys(l)) continue;
        uint16_t vid = 0, pid = 0;
        link_id(l, &vid, &pid);
        OUT(", link %d %04x:%04x held %d", l, vid, pid, umk_keys_held_count(&g_keys[l]));
    }
    OUT("; notes in %lu, dropped %lu\n", (unsigned long)keys::MsgCount(),
        (unsigned long)keys::DropCount());
    {
        uint16_t n = hUsbHostHS.device.CfgDesc.wTotalLength;
        if (n > USBH_MAX_SIZE_CONFIGURATION) n = USBH_MAX_SIZE_CONFIGURATION;
        OUT("direct device %04x:%04x cfg %u B, plain MIDI host can serve it %d\n",
            hUsbHostHS.device.DevDesc.idVendor, hUsbHostHS.device.DevDesc.idProduct,
            hUsbHostHS.device.CfgDesc.wTotalLength,
            umk_direct_ok(hUsbHostHS.device.CfgDesc_Raw, n, USBH_MAX_NUM_INTERFACES,
                          USBH_MAX_NUM_ENDPOINTS, USBH_MAX_SIZE_CONFIGURATION));
    }
    OUT("trace (ms gState enumState):\n");
    for (int i = 0; i < g_tr_n; i++) OUT("  %lu %u %u\n", (unsigned long)g_tr[i].t, g_tr[i].gs, g_tr[i].es);
#undef OUT
    return n < cap ? n : cap;
}

uint32_t RxCount() { return g_rx_count; }
uint32_t TxCount() { return g_tx_count; }

} // namespace lp

namespace
{

void mini_bound()
{
    lp::g_need_init = true;
    lp::g_rx_cable  = -1;
}

/* Unplugged (or the host reset): release what was held; the LEDs are all
 * sent again at the next bind. */
void mini_dropped() { lp::release_held(); }

/* USB-MIDI event packets, 4 bytes each: [cable|CIN, status, data1, data2]. */
void mini_rx(uint8_t* buf, size_t len)
{
    using namespace lp;
    for (size_t i = 0; i + 3 < len; i += 4)
    {
        const uint8_t cin   = buf[i] & 0x0F;
        const int     cable = buf[i] >> 4;
        const uint8_t st    = buf[i + 1] & 0xF0;
        const uint8_t d1    = buf[i + 2];
        const uint8_t d2    = buf[i + 3];
        if (cin != 0x8 && cin != 0x9 && cin != 0xB) continue; /* notes and CCs only */
        if (g_rx_cable < 0) g_rx_cable = cable;
        if (cable != g_rx_cable) continue; /* the same press may come on both */
        if (!g_client)
        {
            uint8_t fwd[16];
            int     fn = 0;
            const llk_route_t r = llk_host_mini_in(&g_lab, buf[i + 1], d1, d2, fwd, &fn);
            lab_queue(fwd, fn);
            if (r == LLK_TOGGLED) { focus_changed(); continue; }
            if (r != LLK_LOCAL) continue;
        }
        const int row = d1 / 10, col = d1 % 10;
        if (row < 1 || row > 9 || col < 1 || col > 9) continue;
        const bool down = (st != 0x80) && d2 > 0;
        if (row == 9)
        {
            if (col <= 8) press(Kind::Top, (uint8_t)(col - 1), 0, down);
        }
        else if (col == 9)
            press(Kind::Side, 0, (uint8_t)(8 - row), down);
        else
            press(Kind::Grid, (uint8_t)(col - 1), (uint8_t)(8 - row), down);
    }
}

} // namespace

/* ============================================================== Launch Control XL */

namespace xl
{

namespace
{

constexpr uint8_t kTemplate = 0x08; /* factory template 1 */
constexpr int     kKnobs    = 24;
constexpr int     kFaders   = 8;
constexpr int     kLeds     = 40;   /* 24 knobs, then 2 rows of 8 buttons */
constexpr uint8_t kUnknown  = 0xFF;

bool     g_need_template = false;
uint8_t  g_knob[kKnobs], g_fader[kFaders];
uint32_t g_knob_changed = 0;
uint8_t  g_fader_changed = 0;

uint8_t g_want[kLeds], g_sent[kLeds];
int     g_scan = 0;

constexpr int kQueue = 32;
Button        g_q[kQueue];
int           g_qhead = 0, g_qtail = 0;

/* A little SysEx reassembly: just enough to see "template changed". */
uint8_t g_sx[16];
int     g_sx_n = 0;

uint16_t g_btn_down = 0; /* bit row*8+col */

void push(uint8_t row, uint8_t col, bool down)
{
    int next = (g_qhead + 1) % kQueue;
    if (next == g_qtail) return;
    g_q[g_qhead] = Button{row, col, down};
    g_qhead      = next;
}

void press(uint8_t row, uint8_t col, bool down)
{
    const uint16_t bit = (uint16_t)(1u << (row * 8 + col));
    g_btn_down = down ? (uint16_t)(g_btn_down | bit) : (uint16_t)(g_btn_down & ~bit);
    push(row, col, down);
}

void sx_byte(uint8_t b)
{
    if (b == 0xF0) g_sx_n = 0;
    if (g_sx_n < (int)sizeof g_sx) g_sx[g_sx_n++] = b;
    if (b == 0xF7)
    {
        /* F0 00 20 29 02 11 77 tt F7: the template changed on the device. */
        if (g_sx_n == 9 && g_sx[5] == 0x11 && g_sx[6] == 0x77 && g_sx[7] != kTemplate)
            g_need_template = true;
        g_sx_n = 0;
    }
}

} // namespace

bool Connected() { return g_xl_link != kNone; }

bool Knob(uint8_t row, uint8_t col, uint8_t* v)
{
    const int i = row * 8 + col;
    if (row > 2 || col > 7) return false;
    *v = g_knob[i];
    if (!(g_knob_changed & (1u << i))) return false;
    g_knob_changed &= ~(1u << i);
    return true;
}

bool Fader(uint8_t col, uint8_t* v)
{
    if (col > 7) return false;
    *v = g_fader[col];
    if (!(g_fader_changed & (1u << col))) return false;
    g_fader_changed = (uint8_t)(g_fader_changed & ~(1u << col));
    return true;
}

uint8_t KnobValue(uint8_t row, uint8_t col) { return (row < 3 && col < 8) ? g_knob[row * 8 + col] : 0; }
uint8_t FaderValue(uint8_t col) { return col < 8 ? g_fader[col] : 0; }

bool PopButton(Button* b)
{
    if (g_qtail == g_qhead) return false;
    *b      = g_q[g_qtail];
    g_qtail = (g_qtail + 1) % kQueue;
    return true;
}

void SetKnobLed(uint8_t row, uint8_t col, uint8_t colour)
{
    if (row < 3 && col < 8) g_want[row * 8 + col] = colour;
}

void SetButtonLed(uint8_t row, uint8_t col, uint8_t colour)
{
    if (row < 2 && col < 8) g_want[24 + row * 8 + col] = colour;
}

/* Called from lp::Poll: template select first, then changed LEDs, one
 * 64-byte transfer at most, never waiting. */
void Flush_()
{
    const int link = g_xl_link;
    if (link == kNone) return;
    uint16_t ep = link_out_size(link);
    if (ep < 16) return;
    if (ep > sizeof g_tx_xl) ep = sizeof g_tx_xl;
    const int max_pk = ep / 4;

    if (g_need_template)
    {
        const uint8_t sx[9] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x77, kTemplate, 0xF7};
        const int     n     = sysex_packets(sx, 9, 0, g_tx_xl);
        if (link_send(link, g_tx_xl, (uint16_t)(4 * n)))
        {
            g_need_template = false;
            for (int i = 0; i < kLeds; i++) g_sent[i] = kUnknown;
        }
        return;
    }

    /* Each LED is its own SysEx (F0 00 20 29 02 11 78 tt idx val F7, 11 bytes
     * = 4 packets), so four LEDs per transfer. */
    int n = 0, sent_idx[4], sent_n = 0;
    for (int k = 0; k < kLeds && n + 4 <= max_pk && sent_n < 4; k++)
    {
        const int     i    = (g_scan + k) % kLeds;
        const uint8_t want = llk_host_xl_led(&g_lab, i, g_want[i]);
        if (want == g_sent[i]) continue;
        const uint8_t sx[11] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x78, kTemplate,
                                (uint8_t)i, want, 0xF7};
        n += sysex_packets(sx, 11, 0, &g_tx_xl[4 * n]);
        sent_idx[sent_n++] = i;
    }
    if (n == 0) return;
    if (!link_send(link, g_tx_xl, (uint16_t)(4 * n))) return;
    for (int j = 0; j < sent_n; j++) g_sent[sent_idx[j]] = llk_host_xl_led(&g_lab, sent_idx[j], g_want[sent_idx[j]]);
    g_scan = (sent_idx[sent_n - 1] + 1) % kLeds;
}

/* Lab B: XL LED changes for the link, up to `budget` LEDs. */
int ClientLeds_(uint8_t* out, int budget)
{
    int n = 0;
    if (g_need_template) { g_need_template = false; for (int i = 0; i < kLeds; i++) g_sent[i] = kUnknown; }
    for (int k = 0; k < kLeds && budget > 0; k++)
    {
        const int i = (g_scan + k) % kLeds;
        if (g_want[i] == g_sent[i]) continue;
        n += llk_enc_xl_led(out + n, i, g_want[i]);
        g_sent[i] = g_want[i];
        g_scan    = (i + 1) % kLeds;
        budget--;
    }
    return n;
}

} // namespace xl

namespace
{

void xl_dropped()
{
    for (int i = 0; i < 16; i++)
        if (xl::g_btn_down & (1u << i)) xl::push((uint8_t)(i / 8), (uint8_t)(i % 8), false);
    xl::g_btn_down = 0;
}

void xl_redraw()
{
    for (int i = 0; i < xl::kLeds; i++) xl::g_sent[i] = xl::kUnknown;
}

void xl_bound()
{
    xl::g_need_template = true;
    for (int i = 0; i < xl::kLeds; i++) xl::g_sent[i] = xl::kUnknown;
}

void xl_rx(uint8_t* buf, size_t len)
{
    using namespace xl;
    for (size_t i = 0; i + 3 < len; i += 4)
    {
        const uint8_t cin = buf[i] & 0x0F;
        const uint8_t st  = buf[i + 1] & 0xF0;
        const uint8_t d1  = buf[i + 2], d2 = buf[i + 3];
        if (cin >= 0x4 && cin <= 0x7)
        {
            const int nb = (cin == 0x5) ? 1 : (cin == 0x6) ? 2 : 3;
            for (int k = 0; k < nb; k++) sx_byte(buf[i + 1 + k]);
            continue;
        }
        if (!g_client && (cin == 0x8 || cin == 0x9 || cin == 0xB))
        {
            uint8_t fwd[4];
            int     fn = 0;
            if (llk_host_xl_in(&g_lab, buf[i + 1], d1, d2, fwd, &fn) == LLK_FORWARDED)
            {
                lab_queue(fwd, fn);
                continue;
            }
        }
        if (cin == 0xB && st == 0xB0)
        {
            int k = -1;
            if (d1 >= 13 && d1 <= 20)      k = d1 - 13;
            else if (d1 >= 29 && d1 <= 36) k = 8 + d1 - 29;
            else if (d1 >= 49 && d1 <= 56) k = 16 + d1 - 49;
            if (k >= 0)
            {
                g_knob[k] = d2;
                g_knob_changed |= 1u << k;
            }
            else if (d1 >= 77 && d1 <= 84)
            {
                g_fader[d1 - 77] = d2;
                g_fader_changed  = (uint8_t)(g_fader_changed | (1u << (d1 - 77)));
            }
        }
        else if (cin == 0x8 || cin == 0x9)
        {
            const bool down = (st == 0x90) && d2 > 0;
            if (d1 >= 41 && d1 <= 44)      press(0, (uint8_t)(d1 - 41), down);
            else if (d1 >= 57 && d1 <= 60) press(0, (uint8_t)(4 + d1 - 57), down);
            else if (d1 >= 73 && d1 <= 76) press(1, (uint8_t)(d1 - 73), down);
            else if (d1 >= 89 && d1 <= 92) press(1, (uint8_t)(4 + d1 - 89), down);
        }
    }
}

} // namespace

/* ================================================================== gamepad */

namespace pad
{

bool Connected()
{
    if (g_client) return (g_cl_presence & LLK_HAS_PAD) != 0;
    for (int l = kHub0; l < kLinks; l++)
        if (link_ready(l) && is_pad(l)) return true;
    return false;
}

uint32_t Buttons()
{
    if (!g_client && llk_host_b_owns(&g_lab)) return 0u;
    return Connected() ? g_pad_buttons : 0u;
}

uint32_t ReportCount() { return g_pad_reports; }

} // namespace pad

/* ================================================================== keyboard */

namespace keys
{

namespace
{

constexpr int kQueue = 128; /* a whole keyboard's release fits */
umk_msg_t     g_q[kQueue];
int           g_qhead = 0, g_qtail = 0;
uint32_t      g_msgs = 0, g_drops = 0;

} // namespace

void Push_(const umk_msg_t* m, int n)
{
    for (int i = 0; i < n; i++)
    {
        const int next = (g_qhead + 1) % kQueue;
        if (next == g_qtail) { g_drops++; continue; } /* full: drop, never block */
        g_q[g_qhead] = m[i];
        g_qhead      = next;
        g_msgs++;
    }
}

bool Connected() { return g_keys_links != 0; }

bool Held()
{
    for (int l = 0; l < kLinks; l++)
        if (is_keys(l) && umk_keys_held_count(&g_keys[l]) > 0) return true;
    return false;
}

bool PopMsg(uint8_t msg[3])
{
    if (g_qtail == g_qhead) return false;
    msg[0]  = g_q[g_qtail].b[0];
    msg[1]  = g_q[g_qtail].b[1];
    msg[2]  = g_q[g_qtail].b[2];
    g_qtail = (g_qtail + 1) % kQueue;
    return true;
}

uint32_t MsgCount() { return g_msgs; }
uint32_t DropCount() { return g_drops; }

} // namespace keys

/* ================================================================== Link client */

namespace xl { int ClientLeds_(uint8_t* out, int budget); }
namespace lp { int client_leds_(uint8_t* out, int budget); }

namespace
{

void cl_isr(uint8_t* data, size_t size, void*)
{
    for (size_t i = 0; i < size; i++)
    {
        const uint32_t next = (g_cl_head + 1) % sizeof g_cl_ring;
        if (next == g_cl_tail) return; /* full: drop, never block */
        g_cl_ring[g_cl_head] = data[i];
        g_cl_head            = next;
    }
}

/* Relayed controller messages go through the same parsers as a device
 * plugged straight in. */
void cl_mini(uint8_t st, uint8_t d1, uint8_t d2, void*)
{
    uint8_t pk[4] = {(uint8_t)(st >> 4), st, d1, d2};
    mini_rx(pk, 4);
}
void cl_xl(uint8_t st, uint8_t d1, uint8_t d2, void*)
{
    uint8_t pk[4] = {(uint8_t)(st >> 4), st, d1, d2};
    xl_rx(pk, 4);
}
void cl_hello(void*)
{
    if (g_cl_outn + 5 <= (int)sizeof g_cl_out) g_cl_outn += llk_enc_hello_reply(g_cl_out + g_cl_outn);
    lp::redraw_();
}
void cl_release(void*) { lp::release_all_(); }
void cl_presence(uint8_t bits, void*)
{
    g_cl_presence = bits;
    if ((bits & LLK_HAS_MINI) && g_mini_link == kNone) { g_mini_link = kVirtual; mini_bound(); }
    else if (!(bits & LLK_HAS_MINI) && g_mini_link == kVirtual) { mini_dropped(); g_mini_link = kNone; }
    if ((bits & LLK_HAS_XL) && g_xl_link == kNone) { g_xl_link = kVirtual; xl_bound(); }
    else if (!(bits & LLK_HAS_XL) && g_xl_link == kVirtual) { xl_dropped(); g_xl_link = kNone; }
    if (!(bits & LLK_HAS_PAD)) g_pad_buttons = 0;
}
void cl_pad(uint32_t mask, void*) { g_pad_buttons = mask; }

void client_poll()
{
    uint8_t  b[256];
    int      n    = 0;
    uint32_t tail = g_cl_tail;
    while (tail != g_cl_head && n < (int)sizeof b)
    {
        b[n++] = g_cl_ring[tail];
        tail   = (tail + 1) % sizeof g_cl_ring;
    }
    g_cl_tail = tail;
    if (n) { g_cl_rx += (uint32_t)n; llk_client_feed(&g_cl, b, n); }

    /* One transfer's worth per millisecond: hello replies, then LEDs. */
    uint8_t out[16 * 11];
    int     o = 0;
    if (g_cl_outn) { memcpy(out, g_cl_out, (size_t)g_cl_outn); o = g_cl_outn; g_cl_outn = 0; }
    o += lp::client_leds_(out + o, 12);
    if (o < (int)sizeof out - 11) o += xl::ClientLeds_(out + o, 1);
    if (o) g_dev.Tx(out, (size_t)o);
}

} // namespace
