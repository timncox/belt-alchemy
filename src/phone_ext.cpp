/**
 * @file phone_ext.cpp
 * @brief HostLink phone block.  See phone_ext.h.
 */

#include "phone_ext.h"

#include <cstring>

namespace phone {

using alchemy::hostlink::FrameWriter;
using alchemy::hostlink::ParsedFrame;
using alchemy::hostlink::Status;

namespace {
inline void Fail(FrameWriter& w, Status s) { w.U8(uint8_t(s)); }
}

void PhoneExtension::Handle(const ParsedFrame& f, FrameWriter& w, uint32_t now_ms)
{
    switch (f.type)
    {
        case kInfo:
        {
            const uint8_t flags = uint8_t((leds_ ? 1u : 0u) | (fw_ ? 2u : 0u) | (press_ ? 4u : 0u));
            w.U8(uint8_t(Status::Ok));
            w.U8(kVersion);
            w.U8(flags);
            w.U16(leds_ ? leds_->Count() : 0u);
            w.Str(layout_);
            return;
        }
        case kScreen:
        {
            if (!leds_) return Fail(w, Status::Unsupported);
            w.U8(uint8_t(Status::Ok));
            w.U32(leds_->Shown());
            w.U16(leds_->Count());
            w.Bytes(leds_->Frame(), 3u * leds_->Count());
            return;
        }
        case kFwList:
        {
            if (!fw_) return Fail(w, Status::Unsupported);
            const int n = fw_->List(now_ms);
            if (n < 0) return Fail(w, Status(uint8_t(-n)));
            w.U8(uint8_t(Status::Ok));
            w.U8(uint8_t(n));
            for (int i = 0; i < n; i++)
            {
                w.Str(fw_->Name(i));
                w.U32(fw_->Size(i));
            }
            if (w.Overflowed())   /* too many names for one body */
            {
                w.Begin(uint8_t(f.type | 0x80u), f.seq);
                return Fail(w, Status::TooLarge);
            }
            return;
        }
        case kFwFlash:
        {
            if (!fw_) return Fail(w, Status::Unsupported);
            if (f.len < 1u || f.len < 1u + f.body[0] || f.body[0] == 0u || f.body[0] > 63u)
                return Fail(w, Status::BadArgs);
            char name[64];
            std::memcpy(name, f.body + 1, f.body[0]);
            name[f.body[0]] = '\0';
            if (std::strchr(name, '/') || std::strchr(name, '\\'))   /* a bare file name */
                return Fail(w, Status::BadArgs);
            w.U8(fw_->Start(name, now_ms));
            return;
        }
        case kFwStatus:
        {
            if (!fw_) return Fail(w, Status::Unsupported);
            float p = fw_->Progress();
            p       = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
            w.U8(uint8_t(Status::Ok));
            w.U8(uint8_t(fw_->State()));
            w.U8(uint8_t(p * 255.0f + 0.5f));
            w.Str(fw_->Error() ? fw_->Error() : "");
            return;
        }
        case kPress:
        {
            if (!press_) return Fail(w, Status::Unsupported);
            if (f.len != 2u || f.body[1] > 2u) return Fail(w, Status::BadArgs);
            w.U8(uint8_t(press_(f.body[0], f.body[1], press_ctx_) ? Status::Ok : Status::BadArgs));
            return;
        }
        default:
            return Fail(w, Status::Unsupported);
    }
}

} // namespace phone
