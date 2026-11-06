# Connecting modules, and what comes when

## Three ways to connect

1. **One module on the USB-C port.** Phone mode, plain USB-C cable. Fastest.
2. **Two modules on a USB-C hub, with the phone or iPad plugged into the hub.**
   The phone is the USB host; iOS sees two class-compliant USB MIDI devices,
   both called "Alchemy Lab". The app pairs each one's in and out by their
   CoreMIDI entity and unique ID, never by name, and tells the modules apart
   by the unit number in Phone INFO (a hash of the STM32's unique ID).
   The modules take no power from USB (their USB-C has no VBUS), so an
   unpowered hub should do; a powered one only matters for charging the
   phone at the same time.
3. **Two modules, each on its own Bluetooth MIDI adapter.** A USB-host
   Bluetooth adapter such as a CME WIDI Uhost on each module's USB-C, paired
   from the app (Connect > Pair a Bluetooth MIDI adapter). Rename the
   adapters in CME's app so they're easy to tell apart; the app still goes
   by unit number. Over Bluetooth the app asks for the screen about 6 times a
   second instead of 15 and writes knobs at most 20 times a second; the
   pitch graph still gets every frame, it just arrives in bigger pieces.
   Files and firmware work but are slow (about 4 KB/s), and the app says so
   before a big one.

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

Not recommended. iOS network MIDI has one session that merges every peer's
input, so replies from two modules would arrive mixed, and the boxes that
bridge USB MIDI to the network (iConnectivity mioXM) are bulky, wired
Ethernet and expensive.

## Linking the two modules (sketch, not built)

The app is the hub; the modules never talk to each other directly. What fits
at control rate:

- Shared key and scale: change either, the other follows.
- One knob or a preset driving both (Retune on both, or Left's Harmony
  driving Right's Doubler).
- Harmonies split across the two: Left sings voices 1 and 2, Right 3 and 4.
- One combined screen on iPad: both pitch graphs on one set of note lanes.

What wireless (or even USB through the phone) rules out: anything that
needs the two in step to the millisecond. Clock, gates and audio stay on
patch cables between the modules; the round trip through the phone is a few ms over USB
and about 25 ms or more over Bluetooth, and it wobbles.

A cable between the two modules' USB-C ports: possible in principle (one
module hosts the other with the Launchpad host code, the 5 V injector, and
usbh_hub_midi.c already takes MIDI devices behind a hub), but that port
then can't talk to the phone. The hub with the phone on it is simpler.

## The mic, and audio

- Built: the mic as a control source (level and pitch).
- Later: record a short clip in the app and put it on the module's card as a
  sample, for firmwares that play samples (Belt doesn't). Over USB a 5 s
  mono clip at 48 kHz takes a few seconds; over Bluetooth, minutes.
- Live mic into Belt today: the phone's audio out (a USB-C audio adapter, or
  the hub's audio jack if it has one) into the module's input with a cable.
- USB audio in Phone mode: feasible, not built. The firmware already has a
  class-compliant USB Audio mode (src/usb_audio.c: 2 in, 2 out, 48 kHz,
  24-bit), but it's one role per boot. Putting the audio interfaces and the
  MIDI interface in one device is standard USB Audio 1; the work is the
  descriptors, endpoint FIFO room on the full-speed core, and allowing four
  interfaces in usbd_ctlreq_uac.c. Then the phone could send its mic to Belt
  and hear Belt back over the one cable. iOS takes one audio interface at a
  time, so with two modules only one would carry audio.

## AUv3 (plan, not built)

An Audio Unit extension (a MIDI processor, "aumi") sharing AlchemyLink, so
AUM, Loopy Pro and other hosts can load it: the modules' knobs as AU
parameters the host can automate, the pad as a view, and MIDI in and out
through the host. The extension reaches the modules with CoreMIDI like the
app does; a host and the app can't both hold the link, so the app gives way
while a host has it. Audio from the modules into iOS effects is a separate
path, through an audio interface or the USB audio above.

## Phases

1. **First TestFlight (this build):** one or two modules over USB, a hub or
   Bluetooth; Belt's screen; split screen on iPad; the XY pad with pressure
   and loops; tilt, turn and mic as controls; knobs, presets, files,
   firmware.
2. **Then:** linking the two modules (shared key, one knob driving both,
   split harmonies, a combined screen), and mic clips to the card.
3. **Later:** the AUv3, and USB audio in Phone mode (firmware work, test on
   hardware first).
