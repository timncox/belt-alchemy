/**
 * @file phone_ext.h
 * @brief HostLink "phone block" (commands 0x70–0x75): what the iPhone app
 *        needs beyond knobs, presets and files.
 *
 * Knobs, settings and presets already travel as HostLink live state and
 * slots (§3), and samples as the filesystem block (§8, 0x50–0x5F).  This
 * block adds the rest, each piece optional:
 *
 *   0x70 PHONE_INFO   → u8 status, u8 version (1), u8 flags, u16 led_count,
 *                       str layout ("alchemy-lab-v2")
 *                       flags: bit0 SCREEN, bit1 firmware switching, bit2 PRESS
 *   0x71 SCREEN       → u8 status, u32 frame, u16 n, u8 rgb[3n]
 *                       the LED frame last shown on the panel (led_mirror.h)
 *   0x72 FW_LIST      → u8 status, u8 count, { str name, u32 size } × count
 *                       the .bin files in /alchemy on the card (FS_NO_CARD
 *                       without one); upload new ones with FS_* first
 *   0x73 FW_FLASH     ← str name   → u8 status
 *                       starts the picker's compare / erase / write / verify
 *                       on that file; the module reboots into it when done
 *   0x74 FW_STATUS    → u8 status, u8 state, u8 progress (0–255), str error
 *                       state: 0 idle, 1 comparing, 2 erasing, 3 writing,
 *                       4 verifying, 5 done (rebooting), 6 same image, 7 error
 *   0x75 PRESS        ← u8 button, u8 gesture (0 tap, 1 down, 2 up) → u8 status
 *                       a panel button, for firmwares that wire it up
 *
 * Unknown or unwired commands answer UNSUPPORTED, so the app shows only
 * what this firmware can do.  Hardware-free: the firmware passes in its LED
 * mirror and a firmware switcher (the picker: picker::Remote() in Belt).
 */

#pragma once

#include <cstdint>

#include "alchemy/host_link/extension.h"
#include "alchemy/host_link/wire.h"
#include "led_mirror.h"

namespace phone {

enum Cmd : uint8_t
{
    kInfo     = 0x70,
    kScreen   = 0x71,
    kFwList   = 0x72,
    kFwFlash  = 0x73,
    kFwStatus = 0x74,
    kPress    = 0x75,
    kLast     = 0x7F,   /* block reserved through here */
};

enum FwState : uint8_t
{
    kFwIdle = 0, kFwComparing, kFwErasing, kFwWriting, kFwVerifying, kFwDone, kFwSame, kFwError,
};

/** Firmware switching from the card, implemented by the picker. */
class IFirmwareSwitcher
{
  public:
    virtual ~IFirmwareSwitcher() = default;
    /** Re-list /alchemy.  Returns the file count, or a negative
     *  hostlink::Status (FS_NO_CARD) when there is no card. */
    virtual int         List(uint32_t now_ms)      = 0;
    virtual const char* Name(int i) const          = 0;
    virtual uint32_t    Size(int i) const          = 0;
    /** Start flashing @p name (as listed).  Returns a hostlink::Status. */
    virtual uint8_t     Start(const char* name, uint32_t now_ms) = 0;
    virtual FwState     State() const              = 0;
    virtual float       Progress() const           = 0;   /* 0..1 */
    virtual const char* Error() const              = 0;
};

using PressFn = bool (*)(uint8_t button, uint8_t gesture, void* ctx);

class PhoneExtension : public alchemy::hostlink::IHostlinkExtension
{
  public:
    static constexpr uint8_t kVersion = 1u;

    PhoneExtension& Screen(const ILedSource& leds) { leds_ = &leds; return *this; }
    PhoneExtension& Firmware(IFirmwareSwitcher& fw) { fw_ = &fw; return *this; }
    PhoneExtension& Press(PressFn fn, void* ctx) { press_ = fn; press_ctx_ = ctx; return *this; }
    PhoneExtension& Layout(const char* name) { layout_ = name; return *this; }

    uint8_t FirstCmd() const override { return kInfo; }
    uint8_t LastCmd() const override { return kLast; }
    void    Handle(const alchemy::hostlink::ParsedFrame& f,
                   alchemy::hostlink::FrameWriter& w, uint32_t now_ms) override;
    const char* DescriptorRootJson() const override { return "\"phone\":{\"v\":1}"; }

  private:
    const ILedSource*  leds_      = nullptr;
    IFirmwareSwitcher* fw_        = nullptr;
    PressFn            press_     = nullptr;
    void*              press_ctx_ = nullptr;
    const char*        layout_    = "alchemy-lab-v2";
};

} // namespace phone
