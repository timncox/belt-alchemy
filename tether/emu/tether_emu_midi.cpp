/* midi_host on the Alchemy Lab emulator: the packets a script's `midi`
 * lines send (emu::midi in the emulator), as if a keyboard (`midi plug`) or
 * a computer (`midi plug computer`) were on the front USB-C. The port takes
 * the same turns as on the module (usb_role.h): a keyboard is found only
 * while the port is a host, a computer only while it is a device, and a
 * message sent before that is lost, as it would be on the cable. */
#include <stdio.h>

#include "midi_host.h"
#include "usb_role.h"

namespace emu { namespace midi {
bool     Pop(uint32_t* pkt);
bool     Plugged();
bool     Computer();
uint32_t Count();
void     Out(uint32_t pkt);
} } // namespace emu::midi

namespace midihost
{
static bool          g_init = false;
static usbrole::Role g_role;
static uint32_t      g_turns = 0;

static bool far_end_seen()
{
    if (!emu::midi::Plugged()) return false;
    return emu::midi::Computer() ? g_role.mode == usbrole::kDevice : g_role.mode == usbrole::kHost;
}

void Init()
{
    g_init = true;
    g_role.Start(0);
}
void Poll(uint32_t now)
{
    if (!g_init) return;
    static bool started = false;
    if (!started) { g_role.Start(now); started = true; }
    if (g_role.Step(now, far_end_seen())) g_turns++;
    if (!far_end_seen())
    {
        uint32_t lost;
        while (emu::midi::Pop(&lost)) {}
    }
}
bool    Pop(uint32_t* pkt) { return g_init && far_end_seen() && emu::midi::Pop(pkt); }
uint8_t Stage()
{
    if (!g_init || !far_end_seen()) return 0;
    return g_role.mode == usbrole::kDevice ? 4 : 3;
}
uint32_t RxCount() { return emu::midi::Count(); }
bool     Send(const uint8_t (*pk)[4], int n)
{
    if (!g_init || !far_end_seen() || g_role.mode != usbrole::kDevice) return false;
    for (int i = 0; i < n; i++)
        emu::midi::Out((uint32_t)pk[i][0] | (uint32_t)pk[i][1] << 8 | (uint32_t)pk[i][2] << 16 | (uint32_t)pk[i][3] << 24);
    return true;
}
uint32_t Dropped() { return 0; }
int      Report(char* b, int cap)
{
    return snprintf(b, (size_t)cap, "tether usb %s (emulator): stage %u, turns %lu\n",
                    g_role.mode == usbrole::kHost ? "host" : "device", Stage(), (unsigned long)g_turns);
}
} // namespace midihost
