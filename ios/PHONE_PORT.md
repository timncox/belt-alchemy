# Giving another Alchemy Lab firmware Phone mode

Belt talks to the Alchemy Screen iPhone app over the front USB-C ("Phone"
USB role: USB MIDI carrying HostLink in SysEx). This is the brief for doing
the same in Smack, Mark, Hypjolin, Tally, Benjolin, Relay, the sequencer and
the Mutable ports. One firmware per session, in that firmware's own repo.

Proven on hardware 2026-11-10: Belt `cbb5678` (branch `claude/iphone-screen`)
on Tim's module answers HELLO, PHONE_INFO and STATE over USB MIDI, shows up in
the app on his iPhone, and a Launchpad on the phone's hub plays it.

## Order

1. Smack (shares the most code with Belt). Prove it end to end before the rest.
2. Mark.
3. Hypjolin, Tally, Benjolin, Relay, seq.
4. mi-alchemy (seven ports, one repo, one pass).

## Before touching anything

- **Start from the build that is on the card, not from `main`.** Read the
  card (`node tools/hostlink-fs.mjs ls /alchemy`), `get` the firmware's
  `.bin`, find its commit (`strings -n 7 x.bin | grep -E '^[0-9a-f]{7}$'`, or
  match its sha256 against `~/tim-os/daisy-sdk/alchemy-lab/*-front.bin`), and
  branch from that commit. Belt's phone branch was cut from release 0.2.0 and
  silently lacked the low-note fixes and chords that were on the card; a build
  from it would have been a regression. `git merge-base --is-ancestor <card
  commit> HEAD` must pass before any card write.
- Grep memory for the firmware first (`project_<name>*.md`,
  `project_alchemy_family.md`, `reference_alchemy_lab_slots.md`). Several of
  these repos keep their real work on a worktree branch with no remote.
- Belt builds against SDK 0.11.0 and uses `Host::Extend` and
  `Host::Transport`. Check the firmware's SDK submodule has both before
  starting; if it does not, the SDK bump is its own job, done first.

## What to copy from belt-alchemy `claude/iphone-screen`

Generic, copy byte-identical (these join the shared-by-copy set):

| File | What it is |
|---|---|
| `src/hlmidi.h` | HostLink frames 7-bit packed into SysEx `F0 7D 48 4C 01 … F7` |
| `src/midi_usb_transport.{h,cpp}` | `IHostTransport` over libDaisy's USB MIDI device mode |
| `src/switch_transport.h` | one `Host` transport, pointed at CDC or MIDI at boot |
| `src/led_mirror.h` | keeps the last LED frame for the app's panel mirror |
| `src/phone_ext.{h,cpp}` | the phone block, HostLink commands 0x70–0x77 |
| `src/ctrl_link.h` | controllers on the phone's hub, forwarded as CTRL |
| `src/usb_shared.{h,cpp}` | `/alchemy/usb.cfg`: adds `phone` (3) and `phone+audio` (4) |
| `src/picker.{h,cpp}` | adds `picker::Remote()` so the app can switch firmware |
| `src/launchpad_core.cpp`, `launchpad_detail.h`, `launchpad.{h,cpp}` | only if the firmware already has Launchpad / XL support |

Phone+Audio (optional, a second pass): `src/usb_phone_audio_desc.h` and the
`usb_audio.{h,c}` / `usbd_ctlreq_uac.c` changes from commit `fd4adfb`. Only
for firmwares that already have Audio mode. Untested on hardware even in Belt.

Belt-only, do not copy: `src/phone_state.h`'s Belt section and the STATE
packing in `belt_alchemy.cpp` (sung pitch, voices, trace). A firmware with
no STATE simply leaves that flag clear in PHONE_INFO.

The commits to read, in order: `d9af93f` (the role), `6ebe805` (STATE, Belt
only), `fd4adfb` (Phone+Audio), `16faac7` (controllers), `72d6e7b` (keyboard).

## Hook-up points in the firmware's main file

Search `belt_alchemy.cpp` for each of these and mirror it:

1. Includes: `led_mirror.h`, `phone_ext.h`, `ctrl_link.h`.
2. Statics: `phone::SwitchTransport front_link`, `hostlink::CdcUsbTransport
   front_cdc`, `hostlink::MidiUsbHostTransport front_midi`,
   `phone::LedMirror<kLedTotal> led_mirror`, `phone::PhoneExtension
   phone_ext`, `phone::CtrlLink ctrl_link`, and `static bool g_phone`.
3. LEDs: `led_mirror.Wrap(hw.strip); hw.leds.Init(led_mirror, hw.Layout());`
   in place of the plain `hw.leds.Init`.
4. Host: `.Extend(phone_ext).Transport(front_link)` on the `Host`, then
   `phone_ext.Screen(led_mirror).Firmware(picker::Remote())`.
5. USB selector: add the `Phone` label (index 3) to the USB port setting, in
   the same order as `usb_shared.h`. Keep "hold B1 at power-up for Mac mode".
6. Boot, after the preset load and `usbshared::Load`: set `g_phone`, then
   `front_midi.Init(front_usb, kFrontUsb, "Alchemy Lab")` +
   `front_link.Use(front_midi)` for Phone, else `front_cdc.Init(...)` +
   `front_link.Use(front_cdc)` for Mac. Launchpad and Audio modes start
   their own stacks and never poll the host.
7. Controllers (only with Launchpad support): `ctrl::Enable(true);
   phone_ext.Controllers(ctrl_link);` in Phone mode, `ctrl::Tick(now)` in the
   poll, and paint when `ctrl::Enabled()`.
8. Makefile: the new `.cpp` files.

Give every performance knob and setting an `.Ident("...")`. The app
addresses fields by that id; a knob without one gets a positional `p0.0`,
which changes if the pages are ever reordered.

## The app, for a firmware that is not Belt

Not yet checked. The app's Screen tab draws Belt's pitch screen from STATE;
without STATE it should fall back to the mirrored panel, and Knobs, Presets,
Samples and Firmware are driven by the descriptor. Its Link features
(`BeltFields` in `ios/AlchemyLink/Sources/AlchemyLink/Linking.swift`) and the
AUv3's parameters are Belt's twelve knobs by name and will do nothing on
another firmware. Find out what the first port actually shows before doing
the second, and fix the app on `claude/iphone-screen` if it assumes Belt.

## Testing

1. Host tests in the firmware's repo.
2. Emulator: `~/tim-os/alchemy-lab/emu`, `make FW=<name>-phone` if a target
   exists (Belt's is `belt-phone`; add one beside it otherwise), then
   `build/<name>-phone/emu --usb phone --hostlink 5071 --nomic` and
   `python3 tools/hostlink_client.py --port 5071 --check`. Always `--nomic`:
   without it the emulator plays the Mac's mic out of the speakers.
3. App in the iOS Simulator against the emulator: launch argument
   `-emulatorMode YES`.
4. Hardware, from the Mac, once Tim has flashed it: a Phone-mode module is a
   USB MIDI device named "Alchemy Lab" with no serial port. Talk to it with
   `~/tim-os/scratch/2026-11-10-alchemy-phone-mode-mac/` (`swiftc -O -o
   hlmidi hlmidi.swift && python3 hello.py ./hlmidi`).

## The card

- **Ask Tim before any card write or flash.** He picks and flashes from the
  module's picker himself.
- The picker lists 16 files and the card has 16 (2026-11-10). A ported
  firmware replaces its own `.bin`; `get` the old one first and keep it under
  `~/tim-os/daisy-sdk/alchemy-lab/` named with its commit.
- `put --overwrite`, then `get` and `cmp`. Never write the card while a
  firmware is flashing.
- The picker sorts files alphabetically. Tell Tim the dot by counting that
  order, not the order they were written.
- `usb.cfg` is one setting for every firmware on the card. A firmware
  without Phone mode that boots with `phone` in it comes up on serial
  (seen with Warps 0.2.2) and leaves the file alone.
- Once a module is in Phone mode the Mac's serial tools cannot reach it.
  Back to Mac: the firmware's USB port setting, or B1 held at power-up.
