/*
 * sysex_transport -- HostLink over USB-MIDI SysEx on the front USB-C, for
 * the Phone role (usb.cfg "phone"): an iPhone app reaches the module
 * through CoreMIDI, which cannot open the CDC port HostLink normally uses.
 * The wire format is sysex_codec.h.
 *
 * libDaisy's USB device is CDC *or* MIDI (usbd_mode), never both, so the
 * role is a boot-time choice: in Phone mode the Mac tools that speak CDC
 * (hostlink.mjs, the web programmer) do not see the module; Chrome's Web
 * MIDI and the iPhone app do. B1 held at power-up still forces Mac mode.
 *
 * Receive: libDaisy's MIDI callback (an interrupt) runs the decoder and
 * drops payload bytes into a single-producer/single-consumer ring, like
 * CdcUsbTransport. Transmit: Write() queues; Pump() (1 ms, control loop)
 * sends up to sysex::kMaxRaw bytes as one message. One instance only.
 *
 * Shared by copy (belt-alchemy first; meant for the SDK fork).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "alchemy/host_link/transport.h"

class SysExUsbTransport : public alchemy::hostlink::IHostTransport
{
  public:
    /* Start USB-MIDI on the front USB-C (FS_EXTERNAL). */
    void Init();

    size_t Read(uint8_t* dst, size_t max) override;
    size_t Write(const uint8_t* src, size_t n) override;
    size_t WriteSpace() const override;
    void   Pump() override;
};
