/* launchpad_core -- the hardware-free half of launchpad.h: the event queues,
 * the knob and fader tables, the gamepad mask and the LED framebuffers that
 * both backends share, and the remote backend (ctrl::, HostLink 0x77 CTRL).
 * The USB host backend is launchpad.cpp. */
#include "launchpad.h"

#include "launchpad_detail.h"

namespace
{
bool    g_remote  = false; /* ctrl::Enable: the remote backend answers */
uint8_t g_present = 0;     /* remote present bits */
} // namespace

/* ============================================================== shared state */

namespace lpd
{

uint8_t lp_want[kLpCells];
uint8_t xl_want[kXlLeds];

volatile uint32_t pad_buttons = 0;
uint32_t          pad_reports = 0;

namespace
{

constexpr int kLpQueue = 64;
lp::Event     g_lp_q[kLpQueue];
volatile int  g_lp_head = 0, g_lp_tail = 0;

uint8_t  g_knob[kXlKnobs], g_fader[8];
uint32_t g_knob_changed  = 0;
uint8_t  g_fader_changed = 0;

constexpr int kXlQueue = 32;
xl::Button    g_xl_q[kXlQueue];
int           g_xl_head = 0, g_xl_tail = 0;

} // namespace

void LpPush(lp::Kind k, uint8_t x, uint8_t y, bool down)
{
    const int next = (g_lp_head + 1) % kLpQueue;
    if (next == g_lp_tail) return; /* full: drop, never block */
    g_lp_q[g_lp_head] = lp::Event{k, x, y, down};
    g_lp_head         = next;
}

void XlKnob(int k, uint8_t v)
{
    if (k < 0 || k >= kXlKnobs) return;
    g_knob[k] = v;
    g_knob_changed |= 1u << k;
}

void XlFader(int col, uint8_t v)
{
    if (col < 0 || col > 7) return;
    g_fader[col]    = v;
    g_fader_changed = (uint8_t)(g_fader_changed | (1u << col));
}

void XlPush(uint8_t row, uint8_t col, bool down)
{
    const int next = (g_xl_head + 1) % kXlQueue;
    if (next == g_xl_tail) return;
    g_xl_q[g_xl_head] = xl::Button{row, col, down};
    g_xl_head         = next;
}

} // namespace lpd

/* ============================================================== Launchpad Mini MK3 */

namespace lp
{

bool Connected() { return g_remote ? (g_present & ctrl::kLaunchpad) != 0 : lpd::UsbLpConnected(); }

bool PopEvent(Event* e)
{
    using namespace lpd;
    if (g_lp_tail == g_lp_head) return false;
    *e        = g_lp_q[g_lp_tail];
    g_lp_tail = (g_lp_tail + 1) % kLpQueue;
    return true;
}

void SetGrid(uint8_t x, uint8_t y, uint8_t col)
{
    if (x < 8 && y < 8) lpd::lp_want[y * 8 + x] = col;
}
void SetSide(uint8_t y, uint8_t col)
{
    if (y < 8) lpd::lp_want[lpd::kLpSide0 + y] = col;
}
void SetTop(uint8_t x, uint8_t col)
{
    if (x < 8) lpd::lp_want[lpd::kLpTop0 + x] = col;
}
void SetLogo(uint8_t col) { lpd::lp_want[lpd::kLpLogo] = col; }

void ClearAll()
{
    for (int i = 0; i < lpd::kLpCells; i++) lpd::lp_want[i] = kOff;
}

} // namespace lp

/* ============================================================== Launch Control XL */

namespace xl
{

bool Connected() { return g_remote ? (g_present & ctrl::kControlXl) != 0 : lpd::UsbXlConnected(); }

bool Knob(uint8_t row, uint8_t col, uint8_t* v)
{
    using namespace lpd;
    const int i = row * 8 + col;
    if (row > 2 || col > 7) return false;
    *v = g_knob[i];
    if (!(g_knob_changed & (1u << i))) return false;
    g_knob_changed &= ~(1u << i);
    return true;
}

bool Fader(uint8_t col, uint8_t* v)
{
    using namespace lpd;
    if (col > 7) return false;
    *v = g_fader[col];
    if (!(g_fader_changed & (1u << col))) return false;
    g_fader_changed = (uint8_t)(g_fader_changed & ~(1u << col));
    return true;
}

uint8_t KnobValue(uint8_t row, uint8_t col)
{
    return (row < 3 && col < 8) ? lpd::g_knob[row * 8 + col] : 0;
}
uint8_t FaderValue(uint8_t col) { return col < 8 ? lpd::g_fader[col] : 0; }

bool PopButton(Button* b)
{
    using namespace lpd;
    if (g_xl_tail == g_xl_head) return false;
    *b        = g_xl_q[g_xl_tail];
    g_xl_tail = (g_xl_tail + 1) % kXlQueue;
    return true;
}

void SetKnobLed(uint8_t row, uint8_t col, uint8_t colour)
{
    if (row < 3 && col < 8) lpd::xl_want[row * 8 + col] = colour;
}

void SetButtonLed(uint8_t row, uint8_t col, uint8_t colour)
{
    if (row < 2 && col < 8) lpd::xl_want[lpd::kXlKnobs + row * 8 + col] = colour;
}

} // namespace xl

/* ================================================================== gamepad */

namespace pad
{

bool Connected() { return g_remote ? (g_present & ctrl::kGamepad) != 0 : lpd::UsbPadConnected(); }

uint32_t Buttons() { return Connected() ? lpd::pad_buttons : 0u; }

uint32_t ReportCount() { return lpd::pad_reports; }

} // namespace pad

/* ================================================================== remote */

namespace ctrl
{

namespace
{

constexpr uint32_t kPadMask = 0x3FFFFu; /* wButtons, LT, RT */

bool     g_lp_held[lpd::kLpCells]; /* what the phone said is down */
uint16_t g_xl_held = 0;            /* bit row * 8 + col */
uint32_t g_last_ms = 0;            /* the last request */

void lp_cell(int i, lp::Kind* k, uint8_t* x, uint8_t* y)
{
    if (i < lpd::kLpSide0)     { *k = lp::Kind::Grid; *x = (uint8_t)(i % 8); *y = (uint8_t)(i / 8); }
    else if (i < lpd::kLpTop0) { *k = lp::Kind::Side; *x = 0; *y = (uint8_t)(i - lpd::kLpSide0); }
    else                       { *k = lp::Kind::Top;  *x = (uint8_t)(i - lpd::kLpTop0); *y = 0; }
}

/* Unplug: release what is held, so nothing stays down. */
void unplug(uint8_t bits)
{
    if (bits & kLaunchpad)
        for (int i = 0; i < lpd::kLpTop0 + 8; i++)
        {
            if (!g_lp_held[i]) continue;
            g_lp_held[i] = false;
            lp::Kind k;
            uint8_t  x, y;
            lp_cell(i, &k, &x, &y);
            lpd::LpPush(k, x, y, false);
        }
    if (bits & kControlXl)
    {
        for (int i = 0; i < 16; i++)
            if (g_xl_held & (1u << i)) lpd::XlPush((uint8_t)(i / 8), (uint8_t)(i % 8), false);
        g_xl_held = 0;
    }
    if (bits & kGamepad) lpd::pad_buttons = 0;
    g_present = (uint8_t)(g_present & ~bits);
}

bool apply_event(const uint8_t* e)
{
    const uint8_t a = e[1], b = e[2], c = e[3];
    switch (e[0])
    {
        case kEvPad:
        {
            if (!(g_present & kLaunchpad)) return false;
            const uint8_t x = b, y = (uint8_t)(c & 0x7F);
            const bool    down = (c & 0x80) != 0;
            int           i;
            if (a == 0 && x < 8 && y < 8) i = y * 8 + x;
            else if (a == 1 && y < 8)     i = lpd::kLpSide0 + y;
            else if (a == 2 && x < 8)     i = lpd::kLpTop0 + x;
            else return false;
            lp::Kind k;
            uint8_t  ex, ey;
            lp_cell(i, &k, &ex, &ey);   /* side: x = 0, top: y = 0, as the USB driver */
            g_lp_held[i] = down;
            lpd::LpPush(k, ex, ey, down);
            return true;
        }
        case kEvKnob:
            if (!(g_present & kControlXl) || a > 2 || b > 7 || c > 127) return false;
            lpd::XlKnob(a * 8 + b, c);
            return true;
        case kEvFader:
            if (!(g_present & kControlXl) || a > 7 || b > 127) return false;
            lpd::XlFader(a, b);
            return true;
        case kEvButton:
        {
            if (!(g_present & kControlXl) || a > 1 || b > 7 || c > 1) return false;
            const uint16_t bit = (uint16_t)(1u << (a * 8 + b));
            g_xl_held = c ? (uint16_t)(g_xl_held | bit) : (uint16_t)(g_xl_held & ~bit);
            lpd::XlPush(a, b, c != 0);
            return true;
        }
        case kEvGamepad:
            if (!(g_present & kGamepad)) return false;
            lpd::pad_buttons = ((uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16)) & kPadMask;
            lpd::pad_reports++;
            return true;
        default:
            return false; /* a type this firmware does not know */
    }
}

} // namespace

void Enable(bool on)
{
    if (!on && g_remote) unplug(kAll);
    g_remote = on;
}

bool Enabled() { return g_remote; }

int Apply(uint8_t present, const uint8_t* events, uint8_t n, uint32_t now_ms)
{
    if (!g_remote) return 0;
    g_last_ms           = now_ms;
    present             = (uint8_t)(present & kAll);
    const uint8_t gone  = (uint8_t)(g_present & ~present);
    if (gone) unplug(gone);
    g_present = present;
    int applied = 0;
    for (int i = 0; i < n; i++)
        if (apply_event(events + 4 * i)) applied++;
    return applied;
}

void Tick(uint32_t now_ms)
{
    if (g_remote && g_present && now_ms - g_last_ms >= kTimeoutMs) unplug(kAll);
}

uint8_t        Present() { return g_remote ? g_present : 0; }
const uint8_t* LpLeds() { return lpd::lp_want; }
const uint8_t* XlLeds() { return lpd::xl_want; }

} // namespace ctrl
