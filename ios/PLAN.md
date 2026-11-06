# Connecting modules, and what comes when

## The setup: a USB-C hub

The default, and what the first TestFlight is for:

- Set each module's USB port to Phone.
- A USB-C hub plugged into the iPhone or iPad, and a USB-C cable from the
  hub to each Alchemy Lab.
- Power the hub from a battery or the case's USB, through the hub's
  power-in (USB-C PD) port, which can also charge the phone.

The phone is the USB host; iOS sees two class-compliant USB MIDI devices,
both called "Alchemy Lab". The app pairs each one's in and out by their
CoreMIDI entity and unique ID, never by name, and tells the modules apart by
the unit number in Phone INFO (a hash of the STM32's unique ID), so "Left"
and "Right" stay put whichever hub port they're in. The modules draw no
power from USB (their USB-C has no VBUS), so the hub's power only runs the
hub and charges the phone.

One module also works on a plain USB-C cable with no hub.

## Other ways to connect (not the default)

**Two modules, each on its own Bluetooth MIDI adapter.** A USB-host
Bluetooth adapter such as a CME WIDI Uhost on each module's USB-C, paired
from the app (Connect > Pair a Bluetooth MIDI adapter). Over Bluetooth the
app asks for the screen about 6 times a second instead of 15 and writes
knobs at most 20 times a second. Files and firmware work but are slow
(about 4 KB/s), and the app says so before a big one.

One Bluetooth adapter on a hub serving both modules doesn't work: the Uhost
takes only the first USB MIDI device and doesn't support hubs.

Unchecked:

- That iOS keeps two Bluetooth LE MIDI devices connected at once. CoreMIDI
  lists each paired device as its own MIDI device and Apple documents no
  limit, but I found no Apple or CME page that says two at once works. The
  app handles any number.
- That a plain USB-C to USB-C cable puts the Uhost into host mode with the
  module (CME's manual shows its own cables).
- That iOS enumerates two identical USB MIDI devices through a hub with
  distinct unique IDs. It should (they are separate USB devices), but this
  wants a test with two modules.

## Wi-Fi

Not planned; the hub does everything. A plain USB Wi-Fi dongle can't work
(the module has no Wi-Fi driver, and its port is full speed only), and iOS
network MIDI merges every peer into one session, so two modules' replies
would mix. A Wi-Fi bridge (a small box per module or one for both, with its
own software) was sketched and set aside on Nov 6.

## Linking the two modules (built, untested on hardware)

The app is the hub; the modules never talk to each other directly.
AlchemyLink/Linking.swift holds the rules and is tested; the app's Link tab
sets them up.

- Shared key and scale: change either, the other follows.
- Follows: one knob drives a knob on the other module over a range you set.
  A follow that would loop back is refused.
- Split harmonies: Left sings plan voices 1 and 2 in its Voice 1/2, Right
  sings 3 and 4; both modules' Voice 3/4 are off and the key is shared.
- One screen: both pitch graphs on one set of note lanes (Left orange,
  Right cyan), on iPad or iPhone.

Writes the app makes for a link are not echoed back to the module they came
from. Everything runs at control rate: clock, gates and audio stay on patch
cables between the modules, because the round trip through the phone is a
few ms over USB and about 25 ms or more over Bluetooth, and it wobbles.

A cable between the two modules' USB-C ports: possible in principle (one
module hosts the other with the Launchpad host code, the 5 V injector, and
usbh_hub_midi.c already takes MIDI devices behind a hub), but that port
then can't talk to the phone. The hub with the phone on it is simpler.

## The mic, and audio

- Built: the mic as a control source (level and pitch).
- Built: record a clip (48 kHz mono WAV, up to 30 s) in Samples, play it
  back, and put it on the module's card. Over USB a 5 s clip takes a few
  seconds; over Bluetooth, minutes. Belt itself doesn't play samples.
- Live mic into Belt without USB audio: the phone's audio out (a USB-C
  audio adapter, or the hub's audio jack) into the module's input.
- Built in firmware, untested on hardware: USB port setting "Phone+Audio"
  (value 4). One composite USB Audio 1 device: audio in and out (2 ch,
  48 kHz, 24-bit) plus the MIDI interface the app uses. The engine keeps
  running: the phone's audio is added to J1/J2 at Belt's input, and Belt's
  output goes to J9/J10 and back to the phone. Checked on the bench in code
  only (descriptor test, 148 checks). To check on hardware: that iOS and
  macOS accept it, dropouts while the app is busy (audio and MIDI share one
  receive FIFO), clock steering, CPU headroom (the engine already peaks
  near 100%), and about 13 ms round trip plus iOS buffering. iOS takes one
  audio interface at a time, so with two modules only one carries audio.

## AUv3 (written, not yet built in Xcode)

An Audio Unit extension (a MIDI processor, "aumi", subtype `AlSc`,
manufacturer `Alch`, shown as "Alchemy Lab: Alchemy Screen"), embedded in
the app, so AUM, Loopy Pro and other hosts can load it. Sources in
`AlchemyAU/`; the target is `AlchemyAU` in `AlchemyScreen/project.yml`. The
parts that don't need Apple frameworks are in AlchemyLink with tests:
`AUParams.swift` (addresses, ranges, CC map), `BeltParamDriver.swift` (one
module's knobs as parameters, coalesced writes, readback) and
`SeqTagTransport.swift` (sharing a module with the app).

- **Parameters**: two groups, Module A and Module B (lowest unit number
  first, the app's Left and Right), each with Belt's twelve performance
  knobs (Key, Scale, Retune, Amount, Harmony, Formant, Voice 1-4, Doubler,
  Spread) and HARD and Mute. Address = slot << 8 | param: A is 0x000-0x00D,
  B is 0x100-0x10D, in that order. Key, Scale and the Voices are indexed
  (named as on the module), the rest 0-100 %, Formant -100 to +100, HARD and
  Mute on/off. Fixed: hosts save automation against them.
- **Host to module**: a change goes out at once as a running-state write
  (HARD and Mute as a button tap, only when the module's state differs),
  one write in flight and at most one per control period, so a fast lane
  never backs up. Every ~400 ms the extension reads the knobs and STATE
  back; a parameter the host changed in the last second is left alone.
- **MIDI**: everything in goes out to the host's MIDI output. CC 20-33 also
  move Module A's fourteen parameters in the order above, CC 52-65 Module
  B's (any channel; for HARD and Mute 64 and up is on).
- **View**: both modules' names and link, the knobs as sliders, HARD and
  Mute as buttons.

Known limits:

- **The app and the extension can both hold a module**, each with its own
  CoreMIDI connection, and both send HostLink. Both hear every reply. A
  reply carries its request's sequence number, but both clients number from
  1, so on their own they would take each other's replies (the test
  `testTwoHostsShareOneModule` shows it happening). The extension tags its
  numbers (top bit set) and drops replies that aren't its own, so it is
  safe; the app ignores the extension's tagged replies until its own count
  passes 32 767 (about 35 minutes of polling), after which a rare
  mismatched reply is possible on the app's side. Polling doubles while both
  are connected. The module has one HostLink parser and one blob transfer,
  so a long frame (descriptor, files) or a running-state write from each at
  the same moment can fail its checksum once and be retried.
  **Recommendation: close the app's connection to the module (or the app)
  while a host uses the extension.** Later, in the app (not done): let go of
  the module while the extension has it (an App Group flag, or pausing when
  the app goes to the background), and start the app's sequence numbers in
  the lower half explicitly.
- **Finding modules by name only.** The extension connects devices whose
  name contains "alchemy". The app also remembers devices that answered
  under another name (a renamed Bluetooth adapter), but that list is in the
  app's own defaults and the extension runs in another process. Keep
  "Alchemy" in the adapters' names, or share the list through an App Group.
- **Session recall**: the host restores the parameters, but a module that
  connects afterwards keeps its own state (it may be mid-song) and the
  parameters follow it.
- Audio from the modules into iOS effects is a separate path, through an
  audio interface or the USB audio above. The extension has one silent
  output bus because some hosts want one on a MIDI processor.

## Phases

1. **First TestFlight:** one or two modules over USB, a hub or Bluetooth;
   Belt's screen; split screen on iPad; the XY pad with pressure and loops;
   tilt, turn and mic as controls; knobs, presets, files, firmware.
2. **Built, same build:** linking (shared key, follows, split harmonies,
   one screen) and mic clips to the card.
3. **Built, test separately:** the AUv3 (try it in AUM first) and the
   Phone+Audio firmware mode (try it on one module before relying on it).
