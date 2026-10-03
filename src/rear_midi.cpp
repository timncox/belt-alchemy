/* MIDI in on the rear header -- see rear_midi.h. */
#include "rear_midi.h"

#include "daisy_core.h"
#include "daisy_seed.h"
#include "per/uart.h"
#include "hid/midi_parser.h"

namespace rearmidi {
namespace {

daisy::UartHandler uart;
daisy::MidiParser  parser;
bool               started = false;

/* The DMA writes here: non-cacheable D2 SRAM, as libDaisy's own MIDI does. */
constexpr size_t kDmaBytes = 64;
uint8_t DMA_BUFFER_MEM_SECTION dma_buf[kDmaBytes];

/* One producer (the UART callback, an interrupt), one consumer (Poll(), the
 * control loop). 256 bytes at 31,250 baud is 80 ms of a saturated cable. */
constexpr uint32_t kRing = 256;
uint8_t            ring[kRing];
volatile uint32_t  ring_w = 0, ring_r = 0;
volatile uint32_t  n_bytes = 0, n_dropped = 0;

void OnRx(uint8_t* data, size_t size, void* ctx, daisy::UartHandler::Result res)
{
    (void)ctx;
    if (res != daisy::UartHandler::Result::OK) return;
    uint32_t w = ring_w;
    for (size_t i = 0; i < size; i++)
    {
        if (w - ring_r >= kRing) { n_dropped = n_dropped + 1u; continue; }
        ring[w % kRing] = data[i];
        w++;
    }
    __asm__ volatile("" ::: "memory");   /* the bytes before the index */
    ring_w  = w;
    n_bytes = n_bytes + (uint32_t)size;
}

void Listen()
{
    started = uart.DmaListenStart(dma_buf, kDmaBytes, OnRx, nullptr)
              == daisy::UartHandler::Result::OK;
}

} // namespace

void Init()
{
    daisy::UartHandler::Config cfg;
    cfg.baudrate      = 31250;
    cfg.periph        = daisy::UartHandler::Config::Peripheral::USART_1;
    cfg.stopbits      = daisy::UartHandler::Config::StopBits::BITS_1;
    cfg.parity        = daisy::UartHandler::Config::Parity::NONE;
    cfg.mode          = daisy::UartHandler::Config::Mode::TX_RX;
    cfg.wordlength    = daisy::UartHandler::Config::WordLength::BITS_8;
    cfg.pin_config.rx = daisy::seed::D14;   /* PB7, header pin 7 */
    cfg.pin_config.tx = daisy::seed::D13;   /* PB6, header pin 8 (idle) */
    parser.Init();
    if (uart.Init(cfg) != daisy::UartHandler::Result::OK) return;
    Listen();
}

void Poll(NoteFn fn)
{
    /* A UART error (an overrun, or noise on an unplugged pin) stops the
     * listen; start it again, as MidiHandler::Listen() does. */
    if (started && !uart.IsListening())
    {
        parser.Reset();
        Listen();
    }

    uint32_t       r = ring_r;
    const uint32_t w = ring_w;
    __asm__ volatile("" ::: "memory");
    for (; r != w; r++)
    {
        daisy::MidiEvent e;
        if (!parser.Parse(ring[r % kRing], &e)) continue;
        if (e.type == daisy::NoteOn && e.data[1] > 0)
            fn(0x90, e.data[0], e.data[1]);
        else if (e.type == daisy::NoteOn || e.type == daisy::NoteOff)
            fn(0x80, e.data[0], 0);
    }
    ring_r = r;
}

uint32_t Bytes() { return n_bytes; }
uint32_t Dropped() { return n_dropped; }

} // namespace rearmidi
