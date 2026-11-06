/**
 * @file ctrl_link.h
 * @brief The phone block's CTRL (0x77) wired to launchpad.h's remote
 *        backend: PhoneExtension::Controllers(ctrl_link) in Phone modes.
 */
#pragma once

#include "launchpad.h"
#include "phone_ext.h"

namespace phone {

class CtrlLink : public IControllers
{
  public:
    static_assert(kLpLeds == ctrl::kLpLeds && kXlLeds == ctrl::kXlLeds, "CTRL LED layout");

    void Apply(uint8_t present, const uint8_t* events, uint8_t n, uint32_t now_ms) override
    {
        ctrl::Apply(present, events, n, now_ms);
    }
    uint8_t        Present() const override { return ctrl::Present(); }
    const uint8_t* LpLeds() const override { return ctrl::LpLeds(); }
    const uint8_t* XlLeds() const override { return ctrl::XlLeds(); }
};

} // namespace phone
