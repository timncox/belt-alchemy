/* The picker's phone side on the emulator (FW_LIST / FW_FLASH): nothing to
 * flash here, as the emulator's own stubs_phone.cpp, which TETHER can't
 * compile (its src/usb_audio.h is TETHER's own, not Belt's). The pots' side
 * (Install / Busy) is the emulator's emu_stubs.cpp. */
#include "alchemy/host_link/wire.h"
#include "phone_ext.h"
#include "picker.h"

namespace
{
class NoFirmware : public phone::IFirmwareSwitcher
{
  public:
    int         List(uint32_t) override { return 0; }
    const char* Name(int) const override { return ""; }
    uint32_t    Size(int) const override { return 0; }
    uint8_t     Start(const char*, uint32_t) override
    {
        return (uint8_t)alchemy::hostlink::Status::Unsupported;
    }
    phone::FwState State() const override { return phone::kFwIdle; }
    float          Progress() const override { return 0.f; }
    const char*    Error() const override { return ""; }
};
NoFirmware g_no_firmware;
} // namespace

namespace picker
{
phone::IFirmwareSwitcher& Remote() { return g_no_firmware; }
void                      Service(uint32_t) {}
} // namespace picker
