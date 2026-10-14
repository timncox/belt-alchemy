/*
 * usb_shared -- the front USB-C's role (Mac / Launchpad / Audio / Link) as
 * ONE setting for every firmware on the card, kept in /alchemy/usb.cfg
 * ("mac", "launchpad", "audio" or "link"). Each firmware still has its own USB port
 * selector; at boot it adopts the card's value, and a change in any
 * firmware's Settings is written back here, so switching firmware in the
 * picker keeps the mode.
 *
 * Shared by copy with smack-alchemy, belt-alchemy and seq-alchemy.
 */
#pragma once
#include <stdint.h>

#include "alchemy/storage/sd_card.h"

namespace usbshared
{

/* -1 = no file (or unreadable), 0 = Mac, 1 = Launchpad, 2 = Audio (the
 * Lab as a USB audio interface, usb_audio.h), 3 = Link (this Lab is "Lab B":
 * a USB-MIDI device on another Lab's hub, sharing its controllers --
 * lab_link.h). A firmware without an Audio or Link choice treats 2 / 3 as Mac. Waits up to
 * timeout_ms for the card to mount. Call from main(), before the loop. */
int Load(alchemy::SdCard& sd, uint32_t timeout_ms);

/* Write the mode. Returns false if the card is not there. */
bool Save(alchemy::SdCard& sd, int mode, uint32_t now_ms);

} // namespace usbshared
