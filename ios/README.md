# Alchemy Screen (iPhone and iPad)

The iPhone or iPad as the Alchemy Lab's screen and controller, over the
front USB-C in Belt's Phone USB mode (HostLink carried in USB MIDI SysEx).

- `AlchemyLink/`: the protocol core in Swift, with tests (`swift test`):
  HostLink, the STATE message, the demo module, and the app's controls
  (routing, tilt, mic level and pitch, the pad's recorded loops).
- `AlchemyScreen.swiftpm/`: the SwiftUI app, openable in Xcode or Swift Playgrounds.
- `AlchemyScreen/`: an XcodeGen project around the same sources, for archiving.
- `AlchemyAU/`: an AUv3 MIDI extension (type aumi, subtype AlSc) embedded in
  the app, so iOS music apps can automate the modules' knobs.
- `TESTFLIGHT.md`: how to get it into TestFlight.
- `PLAN.md`: the ways to connect one or two modules, what's in this build,
  linking, the mic and USB audio, the AUv3, and what's left to test.

What the app does now:

- One module, two on a USB-C hub, or two each on a Bluetooth MIDI adapter.
  Modules are told apart by the unit number they report, so a name ("Left",
  or one you give it) follows a module from cable to adapter.
- On iPad, two modules side by side; on iPhone, a switch at the top.
- Belt's own screen: the sung note against its target, cents, a pitch graph
  over the key's lanes, the harmony voices, the knob a hand is on, HARD and
  mute as buttons (tap latches, hold holds), levels and CPU.
- An XY pad with pressure (Apple Pencil force, or fingertip contact size on
  iPhone), hold or spring-back, and record-to-loop.
- Tilt, roll and turn, and the mic's level and pitch, as controls.
- Every control can go to any knob on either module, over any part of its range.
- Knobs, presets, files and firmware switching, per module.
- Linking two modules: shared key, one knob driving another, harmonies split
  across both, and one combined screen.
- Record a mic clip and put it on the module's card.
- An AUv3 for AUM and other hosts: 14 parameters per module for automation,
  CC 20-33 and 52-65 in, MIDI passed through.
- A demo with one or two pretend modules, so all of it runs in the Simulator.

Not yet built in Xcode or run on a module. The firmware's Phone+Audio USB
mode (audio and the app over one cable) is built but untested on hardware.
