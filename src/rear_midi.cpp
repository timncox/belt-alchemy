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

bool enabled = false;

void Listen()
{
    started = uart.DmaListenStart(dma_buf, kDmaBytes, OnRx, nullptr)
              == daisy::UartHandler::Result::OK;
}

/* The storm guard. libDaisy's DMA1_Stream5_IRQHandler returns WITHOUT
 * clearing anything when no UART owns the stream -- which is the state a
 * UART error leaves behind (the listen stops, the stream's flags stay set).
 * The interrupt then re-fires forever: the control loop runs one
 * instruction between entries and the panel and HostLink die while audio,
 * at a higher slot, plays on. Caught on module 1 (2026-10-21) with the
 * STALL_DIAG build: 3 of 8 samples in this IRQ, the loop parked just after
 * HAL_UART_Receive_DMA. So the vector (the app's table is in SRAM) points
 * here: libDaisy's handler runs as before, then a stopped listener's stream
 * flags are cleared. */
#ifdef STM32H750xx   /* hardware only; the native test has no DMA */
using Isr = void (*)(void);
Isr prev_dma5 = nullptr;

extern "C" void rearmidi_dma5_guard(void)
{
    prev_dma5();
    if (!uart.IsListening())
        DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5
                      | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;
}
#endif

} // namespace

void Enable(bool on)
{
    if (on && !enabled)
    {
        parser.Reset();
        Listen();
        enabled = true;
    }
    else if (!on && enabled)
    {
        if (uart.IsListening()) uart.DmaListenStop();
        started = false;
        enabled = false;
    }
}

void Init()
{
    daisy::UartHandler::Config cfg;
    cfg.baudrate      = 31250;
    cfg.periph        = daisy::UartHandler::Config::Peripheral::USART_1;
    cfg.stopbits      = daisy::UartHandler::Config::StopBits::BITS_1;
    cfg.parity        = daisy::UartHandler::Config::Parity::NONE;
    cfg.mode          = daisy::UartHandler::Config::Mode::RX;
    cfg.wordlength    = daisy::UartHandler::Config::WordLength::BITS_8;
    cfg.pin_config.rx = daisy::seed::D14;   /* PB7, header pin 7 */
    /* No TX pin: libDaisy's InitPins() configures a pin only when its port
     * is not PORTX, which a default Pin() is, so PB6 (header pin 8) stays
     * the reset-state input -- never driven. */
    cfg.pin_config.tx = daisy::Pin();
    parser.Init();
    if (uart.Init(cfg) != daisy::UartHandler::Result::OK) return;
    /* libDaisy leaves the RX pin floating (GPIO_NOPULL). With nothing on
     * the header that reads as noise -- framing errors -- so pull it up to
     * the idle MIDI level. */
#ifdef STM32H750xx
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3u << (7u * 2u))) | (1u << (7u * 2u));
    volatile uint32_t* vt  = reinterpret_cast<volatile uint32_t*>(SCB->VTOR);
    const int          idx = 16 + (int)DMA1_Stream5_IRQn;
    prev_dma5              = reinterpret_cast<Isr>(vt[idx]);
    vt[idx]                = reinterpret_cast<uint32_t>(&rearmidi_dma5_guard);
    __DSB();
#endif
    /* Listening starts with Enable(true): only while MIDI is the source. */
}

void Poll(NoteFn fn)
{
    /* A UART error (an overrun, or noise on an unplugged pin) stops the
     * listen; start it again, as MidiHandler::Listen() does. */
    if (enabled && started && !uart.IsListening())
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
