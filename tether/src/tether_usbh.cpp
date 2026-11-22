/* midi_host on the module: see midi_host.h. Modelled on Belt's
 * launchpad.cpp (the same two classes, the same 256-byte configuration
 * guard), without the controller roles: any MIDI device is a source. */
#include "midi_host.h"

#include <stdio.h>
#include <string.h>

#include "daisy_core.h"
#include "hid/usb_host.h"
#include "usbh_midi.h"
#include "usbh_hub_midi.h"

#include "tether_engine.h"
#include "midi_itf_pick.h"

extern "C" USBH_HandleTypeDef hUsbHostHS; /* libDaisy's host handle */

namespace
{
daisy::USBHostHandle g_usbh;
bool                 g_direct = false;   /* MIDI class active straight on the port */
bool                 g_failed = false;
uint8_t              g_moved = 0;      /* 0.1.1: the MIDI interface number + 1, when it was moved */
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
void on_error(void*) { g_failed = true; }
void on_disconnect(void*)
{
    g_direct = false;
    g_failed = false;
    g_moved  = 0;
    /* What was held when the cable came out is let go by the firmware
     * (Stage drops below 3). */
}
bool via_hub() { return g_usbh.IsActiveClass(USBH_HUB_MIDI_CLASS); }

/* 0.1.1: the core keeps the first USBH_MAX_NUM_INTERFACES interface entries
 * only, and the MIDI class looks for MIDI streaming there alone. A device
 * that lists audio streaming first (the FM-1: audio control, audio in, audio
 * out, then MIDI as interface 3) never had its MIDI opened. Before the core
 * picks a class, put the MIDI interface found in the raw descriptor where the
 * class looks: slot 1 behind an audio control interface (so slot 0 still
 * names the audio class), slot 0 otherwise. A hub is left to the hub class. */
void place_midi_interface()
{
    USBH_CfgDescTypeDef& cfg = hUsbHostHS.device.CfgDesc;
    if (cfg.Itf_Desc[0].bInterfaceClass == 0x09) return;   /* a hub */
    for (int i = 0; i < USBH_MAX_NUM_INTERFACES; i++)
        if (cfg.Itf_Desc[i].bInterfaceClass == 1 && cfg.Itf_Desc[i].bInterfaceSubClass == 3) return;
    uint16_t len = cfg.wTotalLength;
    if (len > USBH_MAX_SIZE_CONFIGURATION) len = USBH_MAX_SIZE_CONFIGURATION;
    midi_pick_t m;
    if (!midi_itf_pick(hUsbHostHS.device.CfgDesc_Raw, len, &m)) return;
    const int slot = (cfg.Itf_Desc[0].bInterfaceClass == 1 && USBH_MAX_NUM_INTERFACES > 1) ? 1 : 0;
    USBH_InterfaceDescTypeDef& d = cfg.Itf_Desc[slot];
    memset(&d, 0, sizeof d);
    d.bLength            = m.itf[0];
    d.bDescriptorType    = m.itf[1];
    d.bInterfaceNumber   = m.itf[2];
    d.bAlternateSetting  = m.itf[3];
    d.bNumEndpoints      = m.neps < USBH_MAX_NUM_ENDPOINTS ? m.neps : USBH_MAX_NUM_ENDPOINTS;
    d.bInterfaceClass    = m.itf[5];
    d.bInterfaceSubClass = m.itf[6];
    d.bInterfaceProtocol = m.itf[7];
    d.iInterface         = m.itf[8];
    for (int e = 0; e < d.bNumEndpoints; e++)
    {
        d.Ep_Desc[e].bLength          = m.ep[e][0];
        d.Ep_Desc[e].bDescriptorType  = m.ep[e][1];
        d.Ep_Desc[e].bEndpointAddress = m.ep[e][2];
        d.Ep_Desc[e].bmAttributes     = m.ep[e][3];
        d.Ep_Desc[e].wMaxPacketSize   = (uint16_t)(m.ep[e][4] | m.ep[e][5] << 8);
        d.Ep_Desc[e].bInterval        = m.ep[e][6];
    }
    g_moved = d.bInterfaceNumber + 1u;
}
} // namespace

namespace midihost
{
void Init()
{
    daisy::USBHostHandle::Config cfg;
    cfg.disconnect_callback   = on_disconnect;
    cfg.class_active_callback = on_class_active;
    cfg.error_callback        = on_error;
    g_usbh.Init(cfg);
    g_usbh.RegisterClass(USBH_MIDI_CLASS);
    g_usbh.RegisterClass(USBH_HUB_MIDI_CLASS);
    HUBMIDI_SetReceiveCallback(on_rx_hub, nullptr);
}

void Poll(uint32_t)
{
    g_usbh.Process();
    /* libDaisy reads the configuration descriptor into 256 bytes, however
     * long the device says it is: ask for no more than fits (Belt). */
    if (hUsbHostHS.gState == HOST_ENUMERATION && hUsbHostHS.EnumState == ENUM_GET_FULL_CFG_DESC
        && hUsbHostHS.RequestState == CMD_SEND
        && hUsbHostHS.device.CfgDesc.wTotalLength > USBH_MAX_SIZE_CONFIGURATION)
        hUsbHostHS.device.CfgDesc.wTotalLength = USBH_MAX_SIZE_CONFIGURATION;
    if (hUsbHostHS.gState == HOST_CHECK_CLASS) place_midi_interface();
    if (hUsbHostHS.gState == HOST_ABORT_STATE) g_failed = true;
}

bool Pop(uint32_t* pkt) { return g_q.Pop(pkt); }

uint8_t Stage()
{
    if (g_direct && g_usbh.IsActiveClass(USBH_MIDI_CLASS)) return 3;
    if (via_hub())
    {
        for (uint8_t d = 0; d < HUBMIDI_MAX_DEVICES; d++)
            if (HUBMIDI_DevReady(&hUsbHostHS, d) && HUBMIDI_DevKind(d) == HUBMIDI_KIND_MIDI) return 3;
        return HUBMIDI_GetInfo().fail_state && !HUBMIDI_Ready(&hUsbHostHS) ? 2 : 1;
    }
    if (g_failed) return 2;
    return hUsbHostHS.device.is_connected ? 1 : 0;
}

uint32_t RxCount() { return g_rx; }
uint32_t Dropped() { return g_q.dropped; }

int Report(char* b, int cap)
{
    int n = 0;
#define OUT(...) do { if (n < cap) n += snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); } while (0)
    OUT("tether usb host: stage %u, direct %d, hub %d, rx %lu, dropped %lu\n", Stage(), (int)g_direct,
        (int)via_hub(), (unsigned long)g_rx, (unsigned long)g_q.dropped);
    if (g_moved) OUT("midi interface %u moved to where the class looks\n", (unsigned)(g_moved - 1u));
    OUT("device %04x:%04x, cfg %u B (fits %d), state %u\n", hUsbHostHS.device.DevDesc.idVendor,
        hUsbHostHS.device.DevDesc.idProduct, hUsbHostHS.device.CfgDesc.wTotalLength,
        (int)(hUsbHostHS.device.CfgDesc.wTotalLength <= USBH_MAX_SIZE_CONFIGURATION),
        (unsigned)hUsbHostHS.gState);
#undef OUT
    return n < cap ? n : cap;
}
} // namespace midihost
