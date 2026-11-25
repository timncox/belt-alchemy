/* midi_host on the Alchemy Lab emulator: the packets a script's `midi`
 * lines send (emu::midi in the emulator), as if a keyboard (`midi plug`) or
 * a computer (`midi plug computer`) were on the front USB-C. The port takes
 * the same turns as on the module (usb_role.h): a keyboard is found only
 * while the port is a host, a computer only while it is a device, and a
 * message sent before that is lost, as it would be on the cable.
 *
 * 0.5.0: USB audio too, while a computer has the port: the script's
 * `usbaudio out` / `usbaudio in` (emu::usbaudio in the emulator) are the
 * far end's stream to TETHER and its ear on TETHER's. AudioTick runs the
 * host's 1 ms packets (44 / 45 frames at 44.1 kHz by its clock, `usbaudio
 * out ... <ppm>` off the Lab's) against the Lab's audio blocks, through
 * the same rings and resamplers as on the module (usb_audio.h). */
#include <stdio.h>

#include "midi_host.h"
#include "usb_audio.h"
#include "usb_role.h"

namespace emu { namespace midi {
bool     Pop(uint32_t* pkt);
bool     Plugged();
bool     Computer();
uint32_t Count();
void     Out(uint32_t pkt);
} } // namespace emu::midi

/* The emulator's --hostlink socket (emu_hostlink.cpp): with `midi plug
 * computer`, the TCP client is that computer's HostLink, the iPhone app in
 * the Simulator or tools/hostlink_client.py (phone_link.h on TETHER). */
namespace emu { namespace usbdev {
void Attach();
void SetRx(void (*fn)(uint8_t*, uint32_t*));
bool Transmit(const uint8_t* p, size_t len);
} } // namespace emu::usbdev

namespace emu {
uint32_t NowUs();
namespace usbaudio {
bool   OutOn();
double OutPpm();
void   OutFrames(int16_t* lr, int frames);
bool   InOn();
void   InFrames(const int16_t* lr, int frames);
} } // namespace emu::usbaudio

namespace midihost
{
static bool          g_init = false;
static usbrole::Role g_role;
static uint32_t      g_turns = 0;

/* Packets from the --hostlink client, in the order they came (firmware thread
 * writes in Service, the audio thread reads in Pop). */
static uint32_t          g_hl_q[1024];
static volatile uint32_t g_hl_w = 0, g_hl_r = 0;
static void hl_rx(uint8_t* buf, uint32_t* len)
{
    for (uint32_t i = 0; i + 4u <= *len; i += 4u)
    {
        if (g_hl_w - g_hl_r >= 1024u) return;
        g_hl_q[g_hl_w & 1023u] = (uint32_t)buf[i] | (uint32_t)buf[i + 1] << 8 | (uint32_t)buf[i + 2] << 16
                                 | (uint32_t)buf[i + 3] << 24;
        g_hl_w = g_hl_w + 1u;
    }
}

static bool far_end_seen()
{
    if (!emu::midi::Plugged()) return false;
    return emu::midi::Computer() ? g_role.mode == usbrole::kDevice : g_role.mode == usbrole::kHost;
}

void Init()
{
    emu::usbdev::SetRx(&hl_rx);
    emu::usbdev::Attach();
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
        g_hl_r = g_hl_w;
    }
}
bool Pop(uint32_t* pkt)
{
    if (!g_init || !far_end_seen()) return false;
    if (emu::midi::Pop(pkt)) return true;
    if (!emu::midi::Computer() || g_role.mode != usbrole::kDevice || g_hl_r == g_hl_w) return false;
    *pkt   = g_hl_q[g_hl_r & 1023u];
    g_hl_r = g_hl_r + 1u;
    return true;
}
uint8_t Stage()
{
    if (!g_init || !far_end_seen()) return 0;
    return g_role.mode == usbrole::kDevice ? 4 : 3;
}
uint8_t  FailWhere() { return 0; }
int      Trace(char* b, int cap) { return Report(b, cap); }
uint32_t TraceSerial() { return 0; }
uint32_t RxCount() { return emu::midi::Count(); }
bool     Send(const uint8_t (*pk)[4], int n)
{
    if (!g_init || !far_end_seen() || g_role.mode != usbrole::kDevice) return false;
    if (!emu::usbdev::Transmit(&pk[0][0], (size_t)n * 4u)) return false;
    for (int i = 0; i < n; i++)
        emu::midi::Out((uint32_t)pk[i][0] | (uint32_t)pk[i][1] << 8 | (uint32_t)pk[i][2] << 16 | (uint32_t)pk[i][3] << 24);
    return true;
}
uint32_t Dropped() { return 0; }

static usbaudio::Link g_link;
static bool           g_out_open = false, g_in_open = false;
static double         g_host_ms = 0.0, g_host_fr = 0.0;

usbaudio::Link& Audio() { return g_link; }

void AudioTick(uint32_t frames)
{
    const bool dev = g_init && far_end_seen() && g_role.mode == usbrole::kDevice;
    const bool out = dev && emu::usbaudio::OutOn();
    const bool in  = dev && emu::usbaudio::InOn();
    if (out != g_out_open) { g_link.OpenOut(out); g_out_open = out; }   /* SET_INTERFACE alt 1 / 0 */
    if (in != g_in_open) { g_link.OpenIn(in); g_in_open = in; }
    if (!out && !in) { g_host_ms = 0.0; return; }
    /* the host's milliseconds in this block, by its clock */
    g_host_ms += (double)frames / 48.0 * (1.0 + emu::usbaudio::OutPpm() * 1e-6);
    const uint32_t now = emu::NowUs();
    while (g_host_ms >= 1.0)
    {
        g_host_ms -= 1.0;
        if (out)
        {
            g_host_fr += 44.1;
            const int k = (int)g_host_fr;
            g_host_fr -= k;
            int16_t lr[2 * usbaudio::kMaxFrames];
            emu::usbaudio::OutFrames(lr, k);
            g_link.OutPacket((const uint8_t*)lr, k * 4, now);
        }
        if (in)
        {
            alignas(4) uint8_t p[usbaudio::kMaxPacketBytes];
            const int bytes = g_link.InPacket(p, now);
            emu::usbaudio::InFrames((const int16_t*)p, bytes / 4);
        }
    }
}
int      Report(char* b, int cap)
{
    return snprintf(b, (size_t)cap, "tether usb %s (emulator): stage %u, turns %lu\n",
                    g_role.mode == usbrole::kHost ? "host" : "device", Stage(), (unsigned long)g_turns);
}
} // namespace midihost
