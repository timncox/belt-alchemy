/* HostLink over USB-MIDI SysEx -- see sysex_transport.h. */
#include "sysex_transport.h"

#include "daisy_seed.h"
#include "hid/usb_midi.h"
#include "sysex_codec.h"

namespace
{

daisy::MidiUsbTransport midi;
sysex::Decoder          decoder;

/* Power-of-two rings indexed by free-running counters. RX: the MIDI
 * interrupt writes, Read() (control loop) reads. TX: control loop only. */
constexpr uint32_t kRxRing = 2048u, kTxRing = 2048u;
uint8_t            rx_ring[kRxRing];
volatile uint32_t  rx_w = 0, rx_r = 0;
uint8_t            tx_ring[kTxRing];
uint32_t           tx_w = 0, tx_r = 0;

uint8_t tx_raw[sysex::kMaxRaw];
uint8_t tx_msg[sysex::kMaxMessage];

void RxByte(uint8_t b, void*)
{
    const uint32_t w = rx_w;
    if (w - rx_r >= kRxRing) return;   /* full: drop; CRC + retry recover */
    rx_ring[w & (kRxRing - 1u)] = b;
    __asm__ volatile("" ::: "memory");
    rx_w = w + 1u;
}

void OnMidi(uint8_t* data, size_t size, void*)
{
    for (size_t i = 0; i < size; i++) decoder.Feed(data[i], RxByte, nullptr);
}

} // namespace

void SysExUsbTransport::Init()
{
    daisy::MidiUsbTransport::Config cfg;
    cfg.periph         = daisy::MidiUsbTransport::Config::EXTERNAL;
    cfg.tx_retry_count = 3;
    midi.Init(cfg);
    midi.StartRx(OnMidi, nullptr);
}

size_t SysExUsbTransport::Read(uint8_t* dst, size_t max)
{
    uint32_t       r = rx_r;
    const uint32_t w = rx_w;
    __asm__ volatile("" ::: "memory");
    size_t n = 0;
    for (; r != w && n < max; r++) dst[n++] = rx_ring[r & (kRxRing - 1u)];
    rx_r = r;
    return n;
}

size_t SysExUsbTransport::Write(const uint8_t* src, size_t n)
{
    size_t k = 0;
    for (; k < n && tx_w - tx_r < kTxRing; k++) tx_ring[(tx_w++) & (kTxRing - 1u)] = src[k];
    return k;
}

size_t SysExUsbTransport::WriteSpace() const { return kTxRing - (tx_w - tx_r); }

void SysExUsbTransport::Pump()
{
    size_t n = 0;
    while (tx_r != tx_w && n < sysex::kMaxRaw) tx_raw[n++] = tx_ring[(tx_r++) & (kTxRing - 1u)];
    if (n == 0) return;
    const size_t len = sysex::Encode(tx_raw, n, tx_msg);
    midi.Tx(tx_msg, len);
}
