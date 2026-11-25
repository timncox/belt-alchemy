/*
 * phone_link -- HostLink (the iPhone app's link, hlmidi.h) over the MIDI
 * that TETHER already speaks when an iPhone, iPad or computer has the front
 * USB-C as its "TETHER" USB-MIDI device (midihost stage 4, the cyan ring).
 *
 * TETHER can't use the shared Phone mode (phone_kit.h): that one opens the
 * port as libDaisy's MIDI device from power-up, and TETHER's port takes turns
 * as a USB host (the FM-1, a keyboard) and as its own composite device
 * (MIDI + audio, tether_usbd.cpp). So this rides on TETHER's device instead:
 * the app's SysEx (F0 7D 48 4C ...) is picked out of the packets the port
 * already receives, and the answers go back through midihost::Send beside
 * Loadout's settings SysEx (F0 7D 54 45 ...) and the CV-in CCs. Nothing to
 * choose: plug the iPhone in where the FM-1 goes and tap TETHER in the app.
 *
 * Feed() runs in the audio callback (where TETHER pops its packets); Read,
 * Write and Pump run in the control loop. One writer and one reader on each
 * ring, so no lock.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "alchemy/host_link/transport.h"
#include "alchemy/host_link/wire.h"
#include "hlmidi.h"
#include "midi_host.h"

namespace tether
{

constexpr size_t PhoneLinkPow2(size_t v)
{
    size_t p = 1u;
    while (p < v) p <<= 1u;
    return p;
}

class PhoneLink : public alchemy::hostlink::IHostTransport
{
  public:
    /* One USB-MIDI event packet as midihost::Pop gives it: cable|CIN in the
     * low byte, then the three MIDI bytes. */
    void Feed(uint32_t pkt)
    {
        const uint8_t b[4] = {(uint8_t)pkt, (uint8_t)(pkt >> 8), (uint8_t)(pkt >> 16),
                              (uint8_t)(pkt >> 24)};
        decoder_.FeedUsb(b, 4u, [this](uint8_t v) {
            const uint16_t w = rx_w_;
            const uint16_t n = (uint16_t)((w + 1u) & (kRx - 1u));
            if (n == rx_r_) { rx_dropped_++; return; }
            rx_[w] = v;
            rx_w_  = n;
        });
    }

    size_t Read(uint8_t* dst, size_t max) override
    {
        size_t k = 0;
        while (k < max && rx_r_ != rx_w_)
        {
            dst[k++] = rx_[rx_r_];
            rx_r_    = (uint16_t)((rx_r_ + 1u) & (kRx - 1u));
        }
        return k;
    }

    size_t Write(const uint8_t* src, size_t n) override
    {
        size_t k = 0;
        while (k < n)
        {
            const uint16_t next = (uint16_t)((tx_w_ + 1u) & (kTx - 1u));
            if (next == tx_r_) break;
            tx_[tx_w_] = src[k++];
            tx_w_      = next;
        }
        return k;
    }

    size_t WriteSpace() const override
    {
        return (size_t)((tx_r_ - tx_w_ - 1u) & (kTx - 1u));
    }

    /* One SysEx message (up to 140 bytes of HostLink) handed to the port at a
     * time; it goes out over the next polls. With nothing on the port as a
     * device, what's waiting is dropped: the app asks again. */
    void Pump() override
    {
        if (midihost::Stage() != 4)
        {
            tx_r_   = tx_w_;
            pend_n_ = 0;
            return;
        }
        if (!pend_n_)
        {
            uint8_t raw[hlmidi::kMaxRawPerMessage];
            size_t  n = 0;
            uint16_t r = tx_r_;
            while (n < sizeof raw && r != tx_w_)
            {
                raw[n++] = tx_[r];
                r        = (uint16_t)((r + 1u) & (kTx - 1u));
            }
            if (!n) return;
            const size_t len = hlmidi::EncodeUsb(raw, n, &pend_[0][0], sizeof pend_);
            if (!len) return;
            pend_n_ = (int)(len / 4u);
            pend_r_ = r;
        }
        if (midihost::Send(pend_, pend_n_))
        {
            tx_r_   = pend_r_;
            pend_n_ = 0;
            sent_++;
        }
    }

    bool Connected() const override { return midihost::Stage() == 4; }

    uint32_t RxMessages() const { return decoder_.Messages(); }
    uint32_t RxDropped() const { return rx_dropped_; }
    uint32_t TxMessages() const { return sent_; }

  private:
    /* Three HostLink frames each way, as midi_usb_transport.h. */
    static constexpr size_t kRx = PhoneLinkPow2(alchemy::hostlink::kMaxWire * 3u);
    static constexpr size_t kTx = kRx;
    static_assert(kRx <= 32768u, "ring indices are 16 bits");
    static constexpr size_t kPend = hlmidi::UsbLen(hlmidi::kMaxRawPerMessage) / 4u;

    hlmidi::Decoder   decoder_;
    uint8_t           rx_[kRx];
    volatile uint16_t rx_w_       = 0u;
    volatile uint16_t rx_r_       = 0u;
    uint32_t          rx_dropped_ = 0u;

    uint8_t  tx_[kTx];
    uint16_t tx_w_ = 0u;
    uint16_t tx_r_ = 0u;

    uint8_t  pend_[kPend][4];
    int      pend_n_ = 0;
    uint16_t pend_r_ = 0u;
    uint32_t sent_   = 0u;
};

} // namespace tether
