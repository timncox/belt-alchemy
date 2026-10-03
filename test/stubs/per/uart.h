/* Native-test stand-in for libDaisy's UartHandler (test_rear_link.cpp):
 * Init() and the circular listen; uart_stub_inject() hands bytes to the
 * listen callback as the DMA interrupt would. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "daisy_seed.h"

namespace daisy {

class UartHandler
{
  public:
    struct Config
    {
        enum class Peripheral { USART_1, USART_2 };
        enum class StopBits { BITS_1 };
        enum class Parity { NONE };
        enum class Mode { RX, TX, TX_RX };
        enum class WordLength { BITS_8 };
        struct { Pin tx; Pin rx; } pin_config;
        Peripheral periph;
        StopBits   stopbits;
        Parity     parity;
        Mode       mode;
        WordLength wordlength;
        uint32_t   baudrate;
    };
    enum class Result { OK, ERR };
    typedef void (*CircularRxCallbackFunctionPtr)(uint8_t*, size_t, void*, Result);

    Result Init(const Config&) { return Result::OK; }
    Result DmaListenStart(uint8_t* b, size_t n, CircularRxCallbackFunctionPtr cb, void* ctx)
    {
        buf = b; size = n; fn = cb; fctx = ctx;
        return Result::OK;
    }
    bool IsListening() const { return fn != nullptr; }

    static inline uint8_t* buf = nullptr;
    static inline size_t   size = 0;
    static inline CircularRxCallbackFunctionPtr fn = nullptr;
    static inline void*    fctx = nullptr;
};

} // namespace daisy

inline void uart_stub_inject(const uint8_t* d, size_t n)
{
    using U = daisy::UartHandler;
    while (n && U::fn)
    {
        const size_t k = n < U::size ? n : U::size;
        std::memcpy(U::buf, d, k);
        U::fn(U::buf, k, U::fctx, U::Result::OK);
        d += k; n -= k;
    }
}
