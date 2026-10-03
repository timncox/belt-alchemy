/* Emulator-internal interfaces shared by emu_board / emu_main / emu_ui. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace emu {

/* Where the QSPI flash image (presets) is kept between runs. Empty = none. */
const std::string& FlashPath();
void               SetFlashPath(const std::string& p);
void               LoadFlash();
uintptr_t          PresetFlashBase();
void               FormatCard();

/* Launchpad / XL / gamepad emulation hooks (emu_stubs.cpp). */
void PadSetButtons(uint32_t mask);

} // namespace emu

/* The firmware's own main(), renamed at compile time (-Dmain=firmware_main). */
int firmware_main(void);
