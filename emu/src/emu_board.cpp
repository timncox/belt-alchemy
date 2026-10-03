/*
 * The emulated Alchemy Lab: time, the panel's pots / buttons / CV, the LED
 * chain, the QSPI flash image (presets persist across runs), the SD card (a
 * formatted RAM disk) and the audio callback hand-off.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "alchemy/hw/alchemy_lab_v2.h"
#include "ff.h"
#include "ram_diskio.h"
#include "emu.h"

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_t0 = Clock::now();

emu::PanelState g_panel;
std::mutex      g_led_mu;
emu::LedFrame   g_leds = {};

daisy::AudioHandle::AudioCallback g_cb = nullptr;
size_t                            g_block = 24;

/* Flash image the preset store lives in; persisted to emu::FlashPath(). */
constexpr size_t kFlashBytes = 256u * 1024u;
alignas(4096) uint8_t g_flash[kFlashBytes];

} // namespace

namespace emu {

uint32_t NowMs()
{
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_t0).count();
}
uint32_t NowUs()
{
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_t0).count();
}
void SleepMs(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

PanelState& Panel() { return g_panel; }

/* CV volts -> the ADC code / raw value. 0 V = mid-scale; +5 V = full scale. */
static float volts_to_raw(float v)
{
    float r = 0.5f + v / 10.0f;
    return r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
}

uint16_t AdcRaw(uint8_t ch)
{
    if (ch >= alchemy::kCvAdcOffset && ch < alchemy::kCvAdcOffset + alchemy::kNumCvInputs)
        return (uint16_t)(volts_to_raw(g_panel.cv_volts[ch - alchemy::kCvAdcOffset].load()) * 65535.0f);
    if (ch < alchemy::kNumPots) return (uint16_t)(g_panel.pot[ch].load() * 65535.0f);
    return 32768u;
}

void RebootRequested(const char* why)
{
    std::fprintf(stderr, "[emu] firmware asked to reboot (%s) -- exiting\n", why);
    std::fflush(stderr);
    std::_Exit(3);
}

void ReadLeds(LedFrame* out)
{
    std::lock_guard<std::mutex> lk(g_led_mu);
    *out = g_leds;
}

daisy::AudioHandle::AudioCallback AudioCb() { return g_cb; }
size_t                            BlockSize() { return g_block; }

uintptr_t PresetFlashBase() { return reinterpret_cast<uintptr_t>(g_flash); }

void LoadFlash()
{
    std::memset(g_flash, 0xFF, sizeof g_flash);
    if (FILE* f = std::fopen(FlashPath().c_str(), "rb"))
    {
        size_t n = std::fread(g_flash, 1, sizeof g_flash, f);
        (void)n;
        std::fclose(f);
    }
}

void FormatCard()
{
    ramdisk::Reset(16384u);   /* 8 MB */
    static uint8_t work[4096];
    f_mkfs("0:", FM_FAT32 | FM_SFD, 512u, work, sizeof work);
}

} // namespace emu

void daisy::QSPIHandle::Persist()
{
    if (emu::FlashPath().empty()) return;
    if (FILE* f = std::fopen(emu::FlashPath().c_str(), "wb"))
    {
        std::fwrite(g_flash, 1, sizeof g_flash, f);
        std::fclose(f);
    }
}

namespace alchemy {

void EmuButton::Sample(uint32_t now_ms)
{
    const bool p = g_panel.button[idx_].load();
    if (p && !pressed_) { rise_ = true; since_ = now_ms; }
    if (!p && pressed_) fall_ = true;
    pressed_ = p;
}

float EmuButton::TimeHeldMs() const
{
    return pressed_ ? (float)(emu::NowMs() - since_) : 0.0f;
}

void EmuStrip::SetPixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (idx >= kLedTotal) return;
    px_[idx][0] = r; px_[idx][1] = g; px_[idx][2] = b;
}

void EmuStrip::Clear() { std::memset(px_, 0, sizeof px_); }

void EmuStrip::Show()
{
    std::lock_guard<std::mutex> lk(g_led_mu);
    std::memcpy(g_leds.rgb, px_, sizeof px_);
}

float EmuCv::Value() const { return emu::volts_to_raw(g_panel.cv_volts[idx_].load()); }

void AlchemyLabV2::Init(daisy::SaiHandle::Config::SampleRate, uint32_t block_size)
{
    block_size_ = block_size;
    g_block     = block_size;
    for (uint8_t i = 0; i < kNumButtons; i++) emu_buttons[i].Bind(i);
    for (uint8_t i = 0; i < kNumCvInputs; i++) cv.slots[i].Bind(i);
    leds.Init(strip, kAlchemyLabV2Layout);
    ProcessAllControls();
}

void AlchemyLabV2::ProcessAllControls()
{
    const uint32_t now = emu::NowMs();
    for (uint8_t i = 0; i < kNumPots; i++) pots[i].SetValue(g_panel.pot[i].load());
    for (uint8_t i = 0; i < kNumButtons; i++) emu_buttons[i].Sample(now);
}

void AlchemyLabV2::StartAudio(daisy::AudioHandle::AudioCallback cb) { g_cb = cb; }

} // namespace alchemy
