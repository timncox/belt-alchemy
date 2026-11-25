#!/usr/bin/env python3
"""
tether_phone_check.py -- TETHER's Phone mode against the Alchemy Lab emulator,
as the iPhone app talks to it over TETHER's own USB-MIDI device: HELLO,
PHONE_INFO, SCREEN, FW_LIST, PRESS (B3 held from the phone shows the jack
page, let go and it goes back), and Loadout's settings SysEx (F0 7D 54 45)
still answered on the same port beside the app's.

The emulator runs a script that plugs a computer in (`midi plug computer`)
with --hostlink <port>: the TCP client is that computer.

    (alchemy-lab/emu) build/custom/<tether>/emu --headless --flash none --hostlink 5090 --script <this folder>/tether_phone.emu &
    python3 tether_phone_check.py --emu-tools <alchemy-lab>/emu/tools --port 5090
"""
import argparse, struct, sys, time, zlib

ap = argparse.ArgumentParser()
ap.add_argument("--emu-tools", required=True)
ap.add_argument("--port", type=int, default=5090)
ap.add_argument("--wait", type=float, default=20.0)
a = ap.parse_args()
sys.path.insert(0, a.emu_tools)
import hostlink_client as hc   # noqa: E402

fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        fails += 1

# The port takes turns as host and device: keep asking until TETHER answers.
deadline = time.monotonic() + a.wait
link = None
while True:
    try:
        if link is None:
            link = hc.Link("127.0.0.1", a.port, 1.5)
        typ, body = link.request(hc.HELLO, bytes([hc.PROTO]))
        break
    except (OSError, TimeoutError):
        if time.monotonic() > deadline:
            print("FAIL no answer from TETHER on port", a.port); sys.exit(1)
        time.sleep(0.3)
link.timeout = 3.0

# Everything else the port sends (Loadout's SysEx), kept raw.
raw = bytearray()
_feed = link.dec.feed
def feed(data):
    raw.extend(data)
    return _feed(data)
link.dec.feed = feed

r = hc.Reader(body)
check(body[0] == 0, "HELLO answers OK")
r.take("B")
proto, board, slots, boot = r.take("BBBB"); r.i += 12
schema, cap, dlen, dcrc = r.take("IIII")
max_body, live_size = r.take("HH")
mid, name, ver, git, sdk = r.str(), r.str(), r.str(), r.str(), r.str()
print(f"  {name} ({mid}) fw {ver}")
check(mid == "tether_alchemy", "it is TETHER")

t, b = link.request(0x70)
check(b[0] == 0, "PHONE_INFO answers OK")
rr = hc.Reader(b[1:]); pver, flags = rr.take("BB"); leds = rr.take("H"); layout = rr.str()
unit = rr.take("I") if rr.left() >= 4 else 0
print(f"  phone block v{pver}, flags 0x{flags:02X}, {leds} LEDs, unit 0x{unit:08X}")
check(flags & 1 and flags & 2 and flags & 4, "SCREEN, FIRMWARE and PRESS offered")
check(leds == 102, "102 LEDs")
check(unit != 0, "a unit id")

def screen():
    t, b = link.request(0x71)
    n = struct.unpack_from("<H", b, 5)[0] if len(b) >= 7 else 0
    return b[0], n, [tuple(b[7 + 3 * i:10 + 3 * i]) for i in range(n)]

st, n, px = screen()
check(st == 0 and n == 102, "SCREEN sends 102 LED colours")
lit = sum(1 for p in px if any(p))
check(lit > 0, f"the panel is lit ({lit} LEDs on)")

t, b = link.request(0x72)
check(b[0] in (0, 1), f"FW_LIST answers ({hc.STATUS.get(b[0])}; the emulator has no card picker)")

t, b = link.request(0x75, bytes([3, 0]))
check(b[0] != 0, "PRESS of a button that isn't there is refused")

# B3 held from the phone: past the tap time TETHER shows the jack page.
t, b = link.request(0x75, bytes([2, 1]))
check(b[0] == 0, "PRESS B3 down answers OK")
time.sleep(0.8)
_, _, held = screen()
t, b = link.request(0x75, bytes([2, 2]))
check(b[0] == 0, "PRESS B3 up answers OK")
time.sleep(0.8)
_, _, after = screen()
d1 = sum(1 for x, y in zip(px, held) if x != y)
d2 = sum(1 for x, y in zip(held, after) if x != y)
print(f"  B3 held: {d1} LEDs changed; let go: {d2} changed back")
check(d1 >= 6, "B3 held from the phone shows another page")
check(d2 >= 6, "letting go from the phone goes back")

# Loadout's settings on the same port, beside the app's HostLink.
raw.clear()
link.sock.sendall(bytes([0xF0, 0x7D, 0x54, 0x45, 0x01, 0xF7]))
end = time.monotonic() + 3.0
while time.monotonic() < end and not (b"\xF0\x7D\x54\x45\x10" in raw and raw.rfind(b"\xF7") > raw.find(b"\xF0\x7D\x54\x45\x10")):
    link.sock.settimeout(0.2)
    try:
        d = link.sock.recv(4096)
        if d: link.dec.feed(d)
    except OSError:
        pass
i = raw.find(b"\xF0\x7D\x54\x45\x10")
check(i >= 0, "Loadout's settings request is still answered (F0 7D 54 45 10 ...)")
if i >= 0:
    vers = bytes(raw[i + 5:i + 13]).rstrip(b"\0").decode(errors="replace")
    print(f"  settings answer from TETHER {vers}")

t, b = link.request(hc.HELLO, bytes([hc.PROTO]))
check(b[0] == 0, "HostLink still answers after the SysEx")

print(f"{'ok' if fails == 0 else 'FAIL'}: {fails} failed")
sys.exit(1 if fails else 0)
