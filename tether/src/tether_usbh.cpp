/* midi_host on the module: see midi_host.h. Modelled on Belt's
 * launchpad.cpp (the same two classes, the same 256-byte configuration
 * guard), without the controller roles: any MIDI device is a source.
 *
 * 0.2.0: the port takes turns as host and device (usb_role.h). As a device it
 * is a class-compliant USB-MIDI device named TETHER, so an iPhone, iPad or
 * Mac can play it; what arrives goes into the same queue as a keyboard's. */
#include "midi_host.h"

#include <stdio.h>
#include <string.h>

#include "daisy_core.h"
#include "sys/system.h"
#include "hid/usb.h"
#include "hid/usb_host.h"
#include "usbd_def.h"
#include "usbd_desc.h"
#include "usbd_ctlreq.h"
#include "usbh_midi.h"
#include "usbh_hub_midi.h"

#include "tether_engine.h"
#include "tether_usbd.h"
#include "usbh_direct.h"
#include "usb_role.h"

#ifndef TETHER_VERSION
#define TETHER_VERSION "0.0.0"
#endif

extern "C" USBH_HandleTypeDef hUsbHostHS; /* libDaisy's host handle */
extern "C" HCD_HandleTypeDef  hhcd_USB_OTG_HS;
extern "C" PCD_HandleTypeDef  hpcd_USB_OTG_HS;

namespace
{
daisy::USBHostHandle g_usbh;
bool                 g_direct = false;   /* MIDI class active straight on the port */
usbdirect::Watch     g_watch;          /* 0.3.1: patience with a device straight on the port */
usbdirect::Log       g_log;            /* 0.3.3: its log, kept across role turns */
usbrole::Role        g_role;
bool                 g_dev_up = false;
uint32_t             g_switches = 0;
uint32_t             g_rx = 0;
tether::Ring<256>    g_q;

void take(const uint8_t* buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i += 4)
    {
        const uint32_t p = (uint32_t)buf[i] | (uint32_t)buf[i + 1] << 8 | (uint32_t)buf[i + 2] << 16
                           | (uint32_t)buf[i + 3] << 24;
        if (!p) continue;   /* padding */
        g_rx++;
        g_q.Push(p);
    }
}

void on_rx_direct(uint8_t* buf, size_t len, void*) { take(buf, len); }
void on_rx_hub(uint8_t dev, uint8_t* buf, size_t len, void*)
{
    if (HUBMIDI_DevKind(dev) == HUBMIDI_KIND_MIDI) take(buf, len);
}

void on_class_active(void*)
{
    if (!g_usbh.IsActiveClass(USBH_MIDI_CLASS)) return;
    USBH_MIDI_SetReceiveCallback(&hUsbHostHS, on_rx_direct, nullptr);
    g_direct = true;
}
void on_error(void*) { g_watch.OnError(); }
void on_connect(void*) { g_log.add(&hUsbHostHS, daisy::System::GetNow(), 'C'); }
void on_disconnect(void*)
{
    g_log.add(&hUsbHostHS, daisy::System::GetNow(), 'U');
    g_direct = false;
    g_watch.Unplugged();
    /* What was held when the cable came out is let go by the firmware
     * (Stage drops below 3). */
}
bool via_hub() { return g_usbh.IsActiveClass(USBH_HUB_MIDI_CLASS); }

/* 0.3.3: the state at the moment a try failed (or a class started), taken
 * then, before the next try or a role turn clears it. */
char     g_snap[2048];
int      g_snap_n      = 0;
uint32_t g_snap_serial = 0;

void snapshot(uint32_t now, bool was_red)
{
    char* b   = g_snap;
    const int cap = (int)sizeof g_snap;
    int n = 0;
#define OUT(...) do { if (n < cap) n += snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); } while (0)
    const USBH_HandleTypeDef& h = hUsbHostHS;
    OUT("at %lu ms: %s, blinks %u, %u failed in a row (red before: %u), names skipped %lu, moved %u, "
        "turns %lu\n",
        (unsigned long)now, h.gState == HOST_CLASS ? "a class started" : "a try failed", (unsigned)g_watch.where,
        (unsigned)g_watch.fails, (unsigned)was_red, (unsigned long)g_watch.names, (unsigned)g_watch.moved,
        (unsigned long)g_switches);
    OUT("core: gState %u (before %u), EnumState %u (before %u), Control %u, RequestState %u, errorcount %u, "
        "address %u, speed %u, connected %u, class %s\n",
        (unsigned)h.gState, (unsigned)g_watch.pre_g, (unsigned)h.EnumState, (unsigned)g_watch.pre_e,
        (unsigned)h.Control.state, (unsigned)h.RequestState, (unsigned)h.Control.errorcount,
        (unsigned)h.device.address, (unsigned)h.device.speed, (unsigned)h.device.is_connected,
        h.pActiveClass ? (const char*)h.pActiveClass->Name : "none");
    const USBH_DevDescTypeDef& d = h.device.DevDesc;
    OUT("device: usb %04x, class %u/%u/%u, ep0 %u, %04x:%04x rel %04x, configs %u\n", d.bcdUSB,
        d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol, d.bMaxPacketSize, d.idVendor, d.idProduct,
        d.bcdDevice, d.bNumConfigurations);
    const USBH_CfgDescTypeDef& c = h.device.CfgDesc;
    OUT("config: %u B, %u interfaces, value %u, attributes %02x, power %u\n", c.wTotalLength,
        c.bNumInterfaces, c.bConfigurationValue, c.bmAttributes, c.bMaxPower);
    for (unsigned i = 0; i < USBH_MAX_NUM_INTERFACES; i++)
    {
        const USBH_InterfaceDescTypeDef& f = c.Itf_Desc[i];
        OUT("slot %u: interface %u alt %u class %u/%u, %u endpoints", i, f.bInterfaceNumber,
            f.bAlternateSetting, f.bInterfaceClass, f.bInterfaceSubClass, f.bNumEndpoints);
        for (unsigned e = 0; e < f.bNumEndpoints && e < USBH_MAX_NUM_ENDPOINTS; e++)
            OUT(" %02x/%u/%u", f.Ep_Desc[e].bEndpointAddress, f.Ep_Desc[e].bmAttributes,
                f.Ep_Desc[e].wMaxPacketSize);
        OUT("\n");
    }
    unsigned len = c.wTotalLength;
    if (len > USBH_MAX_SIZE_CONFIGURATION) len = USBH_MAX_SIZE_CONFIGURATION;
    OUT("config bytes:");
    for (unsigned i = 0; i < len; i++) OUT("%s%02x", (i % 32u) ? " " : "\n", h.device.CfgDesc_Raw[i]);
    OUT("\n");
#undef OUT
    g_snap_n      = n < cap ? n : cap - 1;
    g_snap_serial = g_log.Serial();
}

/* 0.1.1's place_midi_interface (the FM-1's MIDI is interface 3) is now
 * usbdirect::place_midi, called from Watch::After. */

/* ---- the port as a USB-MIDI device (0.2.0), with USB audio (0.5.0):
 * tether_usbd.cpp, TETHER's own class in place of libDaisy's CDC class in
 * MIDI mode ---- */
uint8_t* product_name(USBD_SpeedTypeDef, uint16_t* len)
{
    static uint8_t buf[32];
    USBD_GetString((uint8_t*)"TETHER", buf, len);
    return buf;
}
void dev_rx(uint8_t* buf, uint32_t len) { take(buf, len); }
bool dev_seen() { return g_dev_up && tusbd::Addressed(); }

void host_start()
{
    daisy::USBHostHandle::Config cfg;
    cfg.connect_callback      = on_connect;
    cfg.disconnect_callback   = on_disconnect;
    cfg.class_active_callback = on_class_active;
    cfg.error_callback        = on_error;
    g_usbh.Init(cfg);
    g_usbh.RegisterClass(USBH_MIDI_CLASS);
    g_usbh.RegisterClass(USBH_HUB_MIDI_CLASS);
    HUBMIDI_SetReceiveCallback(on_rx_hub, nullptr);
    g_watch.lg = &g_log;
    g_log.add(nullptr, daisy::System::GetNow(), 'H');
}
void host_stop()
{
    g_usbh.Deinit();
    hhcd_USB_OTG_HS.Instance = nullptr;   /* the shared OTG_HS interrupt then serves the device alone */
    g_direct = false;
    g_watch  = usbdirect::Watch{};
    g_watch.lg = &g_log;
    g_log.add(nullptr, daisy::System::GetNow(), 'D');
}
void dev_start()
{
    HS_Desc.GetProductStrDescriptor = product_name;
    tusbd::Start(dev_rx);
    g_dev_up = true;
}
void dev_stop()
{
    g_dev_up = false;
    tusbd::Stop();
    hpcd_USB_OTG_HS.Instance = nullptr;   /* and the host alone */
}
} // namespace

namespace midihost
{
/* Out to the computer: whole buffer kept until sent, 64 bytes a poll (one
 * full-speed packet), the next only once the last is out. */
static uint8_t g_tx[512];
static int     g_tx_n = 0, g_tx_at = 0;

bool Send(const uint8_t (*pk)[4], int n)
{
    if (g_role.mode != usbrole::kDevice || !dev_seen() || g_tx_at < g_tx_n) return false;
    if (n * 4 > (int)sizeof g_tx) return false;
    memcpy(g_tx, pk, (size_t)n * 4u);
    g_tx_n  = n * 4;
    g_tx_at = 0;
    return true;
}

static void tx_poll()
{
    if (g_tx_at >= g_tx_n) return;
    if (g_role.mode != usbrole::kDevice || !dev_seen()) { g_tx_at = g_tx_n = 0; return; }
    const int len = g_tx_n - g_tx_at < 64 ? g_tx_n - g_tx_at : 64;
    if (tusbd::MidiSend(g_tx + g_tx_at, len)) g_tx_at += len;
}

usbaudio::Link& Audio() { return tusbd::Link(); }
void AudioTick(uint32_t) {}   /* the module's host is real */

void Init()
{
    g_role.Start(daisy::System::GetNow());
    host_start();
}

void Poll(uint32_t now)
{
    const bool seen = g_role.mode == usbrole::kHost ? (bool)hUsbHostHS.device.is_connected : dev_seen();
    if (g_role.Step(now, seen))
    {
        g_switches++;
        if (g_role.mode == usbrole::kDevice) { host_stop(); dev_start(); }
        else { dev_stop(); host_start(); }
        return;
    }
    if (g_role.mode != usbrole::kHost) { tx_poll(); return; }
    /* 0.3.1: Watch around the core (usbh_direct.h; the same loop runs in
     * test/usbh_sim against the real core). */
    if (g_watch.Before(&hUsbHostHS, now)) g_usbh.Process();
    /* libDaisy reads the configuration descriptor into 256 bytes, however
     * long the device says it is: ask for no more than fits (Belt). */
    if (hUsbHostHS.gState == HOST_ENUMERATION && hUsbHostHS.EnumState == ENUM_GET_FULL_CFG_DESC
        && hUsbHostHS.RequestState == CMD_SEND
        && hUsbHostHS.device.CfgDesc.wTotalLength > USBH_MAX_SIZE_CONFIGURATION)
        hUsbHostHS.device.CfgDesc.wTotalLength = USBH_MAX_SIZE_CONFIGURATION;
    const uint32_t restarts = g_watch.restarts;
    const uint32_t serial   = g_log.Serial();
    const uint8_t  red      = g_watch.Failed();
    g_watch.After(&hUsbHostHS, now);
    if (g_log.Serial() != serial) snapshot(now, red);
    if (g_watch.restarts != restarts) g_direct = false;   /* its MIDI class went with it */
}

bool Pop(uint32_t* pkt) { return g_q.Pop(pkt); }

uint8_t Stage()
{
    if (g_role.mode == usbrole::kDevice)
        return g_dev_up && tusbd::Configured() ? 4 : 0;
    if (g_direct && g_usbh.IsActiveClass(USBH_MIDI_CLASS)) return 3;
    if (via_hub())
    {
        for (uint8_t d = 0; d < HUBMIDI_MAX_DEVICES; d++)
            if (HUBMIDI_DevReady(&hUsbHostHS, d) && HUBMIDI_DevKind(d) == HUBMIDI_KIND_MIDI) return 3;
        return HUBMIDI_GetInfo().fail_state && !HUBMIDI_Ready(&hUsbHostHS) ? 2 : 1;
    }
    if (g_watch.Failed()) return 2;
    return hUsbHostHS.device.is_connected ? 1 : 0;
}

uint8_t FailWhere() { return Stage() == 2 && !via_hub() ? g_watch.where : 0; }

uint32_t RxCount() { return g_rx; }
uint32_t Dropped() { return g_q.dropped; }

int Report(char* b, int cap)
{
    int n = 0;
#define OUT(...) do { if (n < cap) n += snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); } while (0)
    OUT("tether usb %s: stage %u, direct %d, hub %d, rx %lu, dropped %lu, turns %lu\n",
        g_role.mode == usbrole::kHost ? "host" : "device", Stage(), (int)g_direct, (int)via_hub(),
        (unsigned long)g_rx, (unsigned long)g_q.dropped, (unsigned long)g_switches);
    if (g_watch.moved) OUT("midi interface %u moved to where the class looks\n", (unsigned)(g_watch.moved - 1u));
    OUT("direct: %lu failed tries (%u in a row, last at step %u), %lu clean starts\n",
        (unsigned long)g_watch.tries, (unsigned)g_watch.fails, (unsigned)g_watch.where,
        (unsigned long)g_watch.restarts);
    OUT("device %04x:%04x, cfg %u B (fits %d), state %u\n", hUsbHostHS.device.DevDesc.idVendor,
        hUsbHostHS.device.DevDesc.idProduct, hUsbHostHS.device.CfgDesc.wTotalLength,
        (int)(hUsbHostHS.device.CfgDesc.wTotalLength <= USBH_MAX_SIZE_CONFIGURATION),
        (unsigned)hUsbHostHS.gState);
    if (g_role.mode == usbrole::kDevice && n < cap) n += tusbd::Report(b + n, cap - n);
#undef OUT
    return n < cap ? n : cap;
}

uint32_t TraceSerial() { return g_snap_serial; }

int Trace(char* b, int cap)
{
    int n = 0;
#define OUT(...) do { if (n < cap) n += snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); } while (0)
    OUT("tether %s, written %lu ms after power-up, %lu failed tries and %lu starts in all\n", TETHER_VERSION,
        (unsigned long)daisy::System::GetNow(), (unsigned long)g_log.fails, (unsigned long)g_log.oks);
    OUT("--- at the last failed try or start ---\n");
    const int k = g_snap_n < cap - n ? g_snap_n : cap - n;
    if (k > 0) { memcpy(b + n, g_snap, (size_t)k); n += k; }
    OUT("--- steps (ms g e ctl err what) ---\n");
    const uint32_t first = g_log.n > usbdirect::kSteps ? g_log.n - usbdirect::kSteps : 0u;
    for (uint32_t i = first; i < g_log.n; i++)
    {
        const usbdirect::Step& s = g_log.steps[i % usbdirect::kSteps];
        OUT("%lu %u %u %u %u %c\n", (unsigned long)s.t, s.g, s.e, s.ctl, s.err, s.ev);
    }
#undef OUT
    return n < cap ? n : cap;
}
} // namespace midihost
