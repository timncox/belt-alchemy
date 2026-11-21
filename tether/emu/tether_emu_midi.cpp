/* midi_host on the Alchemy Lab emulator: the packets a script's `midi`
 * lines send (emu::midi in the emulator), as if a device were plugged into
 * the front USB-C. */
#include <stdio.h>

#include "midi_host.h"

namespace emu { namespace midi {
bool     Pop(uint32_t* pkt);
bool     Plugged();
uint32_t Count();
} } // namespace emu::midi

namespace midihost
{
static bool g_init = false;
void     Init() { g_init = true; }
void     Poll(uint32_t) {}
bool     Pop(uint32_t* pkt) { return g_init && emu::midi::Pop(pkt); }
uint8_t  Stage() { return !g_init ? 0 : emu::midi::Plugged() ? 3 : 0; }
uint32_t RxCount() { return emu::midi::Count(); }
uint32_t Dropped() { return 0; }
int      Report(char* b, int cap) { return snprintf(b, (size_t)cap, "tether usb host: emulator\n"); }
} // namespace midihost
