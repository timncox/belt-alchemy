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

- Two modules on a USB-C hub plugged into the phone (the default; see PLAN.md),
  one module on a plain cable, or two each on a Bluetooth MIDI adapter.
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
- Controllers on the hub (Launchpad Mini MK3, Launch Control XL, Haute42,
  any MIDI device): sent to a module like its Launchpad mode, or learned and
  mapped in the app, including MIDI out to a Daisy Patch.
- Routing: any USB MIDI device on the hub to any other, or to a module,
  through the app (PLAN.md, "Everything talks over USB").
- Linking two modules: shared key, one knob driving another, harmonies split
  across both, and one combined screen.
- Record a mic clip and put it on the module's card.
- An AUv3 for AUM and other hosts: 14 parameters per module for automation,
  CC 20-33 and 52-65 in, MIDI passed through.
- A demo with one or two pretend modules, so all of it runs in the Simulator.

## Testing without hardware

Nothing below has been compiled in Xcode yet (AlchemyLink's tests run on
Linux; the app's files are only syntax-checked).

**Demo in the Simulator.** Run the app in an iPhone or iPad simulator,
then Connect > Try two demo modules: two pretend modules (Left and Right,
distinct unit numbers) inside the app, so linking and two-module routes work.

**On-screen devices.** In the demo and with the emulator, Controls >
On-screen devices (on by default) adds a Launchpad Mini MK3 (lit with the
module's CTRL colours), a Launch Control XL, a gamepad and a Daisy Patch
(four knobs as CC 21-24, a gate as note C4, and a monitor of the last 50
MIDI messages it gets). They reach the app the way the real ones do, under
the real ones' names, so routes, Learn and mappings made with them carry
over. On iPad they're the Devices tab; on iPhone, Controls > On-screen
devices > Play them. Give the gamepad and the Daisy Patch the names the
real ones show in Controls.

**The module emulator.** The Alchemy Lab emulator runs the Phone firmware
on the Mac and serves its USB MIDI stream (plain SysEx bytes) on a TCP
port. See the alchemy-lab repo's `emu/README.md`, "Phone mode and the iOS
app". In zsh:

    git -C ~/tim-os/belt-alchemy worktree add .claude/worktrees/iphone-screen claude/iphone-screen
    git -C ~/tim-os/belt-alchemy/.claude/worktrees/iphone-screen submodule update --init --recursive
    cd <alchemy-lab checkout, branch claude/emu-hostlink>/emu && make FW=belt-phone
    build/belt-phone/emu --usb phone --hostlink 5071 --flash ~/.alchemy-emu/left.bin
    build/belt-phone/emu --usb phone --hostlink 5072 --flash ~/.alchemy-emu/right.bin --nomic

Then in the app, Connect > Emulator > Connect to the module emulator. Left
is 127.0.0.1:5071 and Right 127.0.0.1:5072 (editable; on a real iPhone or
iPad, type the Mac's address). The app retries every 2 s until they answer,
and reconnects next launch. The emulator carries no audio in Phone+Audio
mode, and firmware switching lists nothing.

Not yet built in Xcode or run on a module. The firmware's Phone+Audio USB
mode (audio and the app over one cable) is built but untested on hardware.
