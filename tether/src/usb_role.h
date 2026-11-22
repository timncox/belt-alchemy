/* usb_role -- which way round the front USB-C runs (TETHER 0.2.0).
 *
 * The Lab has one USB port for this (the Patch SM's USB HS pins on the front
 * USB-C) and no 5 V on it, so nothing in the hardware says whether a keyboard
 * or a computer is on the other end. The port takes turns: as a host it
 * waits for a device to show up (a keyboard's D+ pull-up); as a device it
 * waits for a computer to set it up (an iPhone, iPad or Mac giving it an
 * address). Whichever happens, it stays that way until that end goes away.
 * Two hosts or two devices on one cable do nothing to each other, so the
 * turns are harmless.
 *
 * Pure: the module (tether_usbh.cpp) and the emulator (emu/) feed it what
 * they see and switch the port when Step says so. */
#pragma once
#include <stdint.h>

namespace usbrole
{
enum Mode : uint8_t { kHost = 0, kDevice = 1 };

constexpr uint32_t kHostTurnMs   = 1500u;   /* host, nothing plugged in: then try as a device */
constexpr uint32_t kDeviceTurnMs = 2500u;   /* device, no computer set it up: then try as a host */
constexpr uint32_t kGoneMs       = 1500u;   /* the far end quiet this long after it was there: it's gone */

struct Role
{
    Mode     mode  = kHost;
    uint32_t since = 0;       /* when this turn (or the far end's last sign) began */
    bool     held  = false;   /* the far end has been seen this turn */

    void Start(uint32_t now) { mode = kHost; since = now; held = false; }

    /* now: ms. seen: as host, a device is attached; as device, a computer has
     * the port set up (addressed or configured). Returns true when the port
     * should switch to the other role (mode is already the new one). */
    bool Step(uint32_t now, bool seen)
    {
        if (seen)
        {
            held  = true;
            since = now;
            return false;
        }
        const uint32_t wait = held ? kGoneMs : mode == kHost ? kHostTurnMs : kDeviceTurnMs;
        if ((uint32_t)(now - since) < wait) return false;
        mode  = mode == kHost ? kDevice : kHost;
        since = now;
        held  = false;
        return true;
    }
};
} // namespace usbrole
