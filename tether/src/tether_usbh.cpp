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

extern "C" USBH_HandleTypeDef hUsbHostHS; /* libDaisy's host handle */

namespace
{
daisy::USBHostHandle g_usbh;
bool                 g_direct = false;   /* MIDI class active straight on the port */
bool                 g_failed = false;
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
    /* What was held when the cable came out is let go by the firmware
     * (Stage drops below 3). */
}
bool via_hub() { return g_usbh.IsActiveClass(USBH_HUB_MIDI_CLASS); }
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
    OUT("device %04x:%04x, cfg %u B (fits %d), state %u\n", hUsbHostHS.device.DevDesc.idVendor,
        hUsbHostHS.device.DevDesc.idProduct, hUsbHostHS.device.CfgDesc.wTotalLength,
        (int)(hUsbHostHS.device.CfgDesc.wTotalLength <= USBH_MAX_SIZE_CONFIGURATION),
        (unsigned)hUsbHostHS.gState);
#undef OUT
    return n < cap ? n : cap;
}
} // namespace midihost
