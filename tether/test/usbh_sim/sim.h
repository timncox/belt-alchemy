/* A pretend USB port for ST's host core, so TETHER's direct path (and the hub
 * path) can run on the laptop with the real usbh_core.c, usbh_ctlreq.c,
 * usbh_midi.c and Belt's usbh_hub_midi.c. sim.cpp stands in for libDaisy's
 * usbh_conf.c (the HAL underneath): the core's SETUP, IN and OUT go to a
 * modelled device, which can be slow or deaf the ways a real one can. */
#pragma once
#include <stdint.h>
#include "usbh_core.h"

struct SimDev
{
    /* what it says */
    const uint8_t* dev_desc  = nullptr;   /* 18 bytes */
    const uint8_t* cfg       = nullptr;
    uint16_t       cfg_len   = 0;
    const uint8_t* strings[4] = {};      /* 0: languages, 1..3 */
    uint8_t        midi_in_ep = 0x81;

    /* how it behaves */
    uint32_t addr_lag_ms     = 0;   /* the new address takes hold this long after SET_ADDRESS's status */
    uint32_t deaf_after_reset_ms = 0;   /* nothing answered this long after a bus reset */
    uint32_t drop_first      = 0;   /* the first N SETUPs get no answer at all */
    uint32_t drop_every      = 0;   /* then every Nth (0: none) */
    bool     dead            = false;   /* never answers (pulled up, nothing behind it) */

    /* what happened */
    uint8_t  addr = 0, config = 0;
    uint32_t setups = 0, resets = 0, sets_config = 0, strings_asked = 0;
};

namespace sim
{
uint32_t Now();
void     Advance(USBH_HandleTypeDef* ph, uint32_t ms);   /* the bus runs on; the core's timer ticks */

/* Straight on the port, or behind a hub (on its port 1). */
void Plug(USBH_HandleTypeDef* ph, SimDev* dev, bool via_hub = false);
void Unplug(USBH_HandleTypeDef* ph);
void QueueMidi(const uint8_t pkt[4]);   /* the device sends a USB-MIDI packet */
void Reset();                           /* nothing plugged, time 0 */
SimDev* Hub();                          /* the modelled hub (its counters) */
} // namespace sim
