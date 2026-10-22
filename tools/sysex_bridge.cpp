/*
 * sysex_bridge -- a Mac pty <-> USB-MIDI SysEx bridge for a module in the
 * Phone role (src/sysex_transport.h). It opens a pseudo-terminal, prints its
 * path, and relays: bytes written to the pty go to the module as SysEx
 * (src/sysex_codec.h), and SysEx from the module comes back out of the pty.
 * So the CDC tools work unchanged against a Phone-mode module:
 *
 *   clang++ -std=c++17 -O2 -Isrc tools/sysex_bridge.cpp -o build/sysex_bridge \
 *       -framework CoreMIDI -framework CoreFoundation
 *   build/sysex_bridge [name-substring]        # default: first match of
 *                                              # "Daisy", "Alchemy", "Belt"
 *   node tools/hostlink-fs.mjs -p /dev/ttysNNN info
 *
 * With no matching device it lists every MIDI source and destination.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

#include <string>

#include "sysex_codec.h"

static int             g_master = -1;
static MIDIPortRef     g_out_port;
static MIDIEndpointRef g_dest;
static sysex::Decoder  g_decoder;

static std::string EndpointName(MIDIEndpointRef e)
{
    CFStringRef s = nullptr;
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s);
    if (!s) return "?";
    char buf[256] = {};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

static void ToPty(uint8_t b, void*)
{
    (void)!write(g_master, &b, 1);
}

static void ReadProc(const MIDIPacketList* list, void*, void*)
{
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++)
    {
        for (UInt16 j = 0; j < p->length; j++) g_decoder.Feed(p->data[j], ToPty, nullptr);
        p = MIDIPacketNext(p);
    }
}

static void SendRaw(const uint8_t* raw, size_t n)
{
    uint8_t msg[sysex::kMaxMessage];
    const size_t len = sysex::Encode(raw, n, msg);
    Byte          buf[1024];
    MIDIPacketList* pl = (MIDIPacketList*)buf;
    MIDIPacket*     pk = MIDIPacketListInit(pl);
    MIDIPacketListAdd(pl, sizeof buf, pk, 0, len, msg);
    MIDISend(g_out_port, g_dest, pl);
}

static bool Matches(const std::string& name, const char* want)
{
    if (want) return name.find(want) != std::string::npos;
    return name.find("Daisy") != std::string::npos || name.find("Alchemy") != std::string::npos
           || name.find("Belt") != std::string::npos;
}

int main(int argc, char** argv)
{
    const char* want = argc > 1 ? argv[1] : nullptr;
    MIDIClientRef client;
    MIDIClientCreate(CFSTR("sysex_bridge"), nullptr, nullptr, &client);

    MIDIEndpointRef src = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++)
        if (Matches(EndpointName(MIDIGetSource(i)), want)) { src = MIDIGetSource(i); break; }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++)
        if (Matches(EndpointName(MIDIGetDestination(i)), want)) { g_dest = MIDIGetDestination(i); break; }
    if (!src || !g_dest)
    {
        fprintf(stderr, "no matching MIDI device. sources:\n");
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++)
            fprintf(stderr, "  %s\n", EndpointName(MIDIGetSource(i)).c_str());
        fprintf(stderr, "destinations:\n");
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++)
            fprintf(stderr, "  %s\n", EndpointName(MIDIGetDestination(i)).c_str());
        return 1;
    }

    int  slave = -1;
    char path[128];
    if (openpty(&g_master, &slave, path, nullptr, nullptr) != 0) { perror("openpty"); return 1; }
    struct termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);

    MIDIPortRef in_port;
    MIDIInputPortCreate(client, CFSTR("in"), ReadProc, nullptr, &in_port);
    MIDIPortConnectSource(in_port, src, nullptr);
    MIDIOutputPortCreate(client, CFSTR("out"), &g_out_port);

    printf("%s  <->  %s / %s\n", path, EndpointName(src).c_str(), EndpointName(g_dest).c_str());
    fflush(stdout);

    /* The pty side: whatever a tool writes, send on as SysEx. */
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    (void)loop;
    uint8_t buf[sysex::kMaxRaw];
    for (;;)
    {
        const ssize_t n = read(g_master, buf, sizeof buf);
        if (n > 0) SendRaw(buf, (size_t)n);
        else usleep(1000);
    }
}
