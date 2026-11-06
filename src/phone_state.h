/**
 * @file phone_state.h
 * @brief Hardware-free pieces behind the phone block's STATE reply
 *        (phone_ext.h, 0x76): which pot a hand last moved, a short trace of
 *        a value sampled every control frame, and Belt's sung pitch in
 *        cents.  Header-only, so the host tests use the same code.
 */

#pragma once

#include <cmath>
#include <cstdint>

#include "alchemy/host_link/wire.h"

namespace phone {

/** Notices the last pot moved by hand from the pager's stored values.
 *  A preset load moves many at once, so a frame where more than two move
 *  counts as no touch. */
template <uint8_t kPages, uint8_t kPots>
class TouchTracker
{
  public:
    static constexpr float kStep = 0.006f;   /* ~ADC noise floor, in travel */

    template <typename StoredFn>
    void Update(uint32_t now_ms, StoredFn stored)
    {
        uint8_t moved = 0, mp = 0xFFu, mq = 0xFFu;
        for (uint8_t p = 0; p < kPages; p++)
            for (uint8_t q = 0; q < kPots; q++)
            {
                const float v = stored(p, q);
                if (!primed_) { seen_[p][q] = v; continue; }
                if (std::fabs(v - seen_[p][q]) > kStep)
                {
                    seen_[p][q] = v;
                    moved++;
                    mp = p;
                    mq = q;
                }
            }
        primed_ = true;
        if (moved >= 1u && moved <= 2u)
        {
            page_ = mp;
            pot_  = mq;
            at_   = now_ms;
        }
    }

    uint8_t  Page() const { return page_; }
    uint8_t  Pot() const { return pot_; }
    uint16_t AgeMs(uint32_t now_ms) const
    {
        if (pot_ == 0xFFu) return 0xFFFFu;
        const uint32_t a = now_ms - at_;
        return a > 0xFFFEu ? uint16_t(0xFFFEu) : uint16_t(a);
    }

  private:
    float    seen_[kPages][kPots] = {};
    bool     primed_ = false;
    uint8_t  page_   = 0xFFu, pot_ = 0xFFu;
    uint32_t at_     = 0;
};

/** The last N samples of one value, one per control frame, numbered.  The
 *  phone asks for what came after the last number it has. */
template <uint16_t N>
class Trace
{
    static_assert(N > 0u && (N & (N - 1u)) == 0u, "N must be a power of two");

  public:
    static constexpr int16_t kNone = INT16_MIN;   /* e.g. unvoiced */

    void     Push(int16_t v)
    {
        buf_[seq_ % N] = v;
        seq_++;
        if (count_ < N) count_++;
    }
    uint16_t Last() const { return uint16_t(seq_ - 1u); }

    /** u16 last, u8 n, then n × s16 (oldest first): the samples after
     *  @p since, at most @p max_n of the newest. */
    void Write(alchemy::hostlink::FrameWriter& w, uint16_t since, uint8_t max_n) const
    {
        uint16_t n = uint16_t(seq_ - 1u - since);   /* new since the phone's last */
        if (n > max_n) n = max_n;
        if (n > count_) n = count_;
        w.U16(Last());
        w.U8(uint8_t(n));
        for (uint16_t i = 0; i < n; i++)
            w.U16(uint16_t(buf_[uint16_t(seq_ - n + i) % N]));
    }

  private:
    int16_t  buf_[N] = {};
    uint16_t seq_    = 0;   /* samples pushed, wrapping */
    uint16_t count_  = 0;   /* how many buf_ holds, up to N */
};

/** Belt's status readback ("note10:cents:voiced:mask") as the sung pitch in
 *  MIDI cents (A4 = 6900), to the cent: the correction target is a whole
 *  note, and the engine gives the distance from it in cents. */
inline int16_t SungCents(int note10, int cents, bool voiced)
{
    if (!voiced) return Trace<1>::kNone;
    const long target = std::lround((note10 * 10.0 - cents) / 100.0);
    long       c      = target * 100 + cents;
    if (c < 0) c = 0;
    if (c > 12799) c = 12799;
    return int16_t(c);
}

} // namespace phone
