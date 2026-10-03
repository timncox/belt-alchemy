# belt-emu — Belt on an emulated Alchemy Lab

The real firmware, on the Mac. `src/belt_alchemy.cpp`, the Belt engine and the
Alchemy SDK framework (pages, Settings, presets, the control loop, the CV
matrix, the LED renderer) are compiled **unchanged**; only the board is
replaced:

| Replaced | By |
|---|---|
| `alchemy/hw/alchemy_lab_v2.h` (AlchemyLabV2) | `include/alchemy/hw/alchemy_lab_v2.h` — pots, B1–B3, J3–J8 and the 102-LED chain are the emulator panel |
| libDaisy (`daisy_seed.h`, `hid/usb.h`, `util/CpuLoadMeter.h`, HAL) | `include/` — real time, ADC from the panel, QSPI = a flash image file |
| QSPI flash at 0x90760000 | `~/.alchemy-emu/belt-flash.bin` (presets survive restarts; `--flash none` = RAM) |
| SD card | a formatted 8 MB RAM disk under the real FatFS (the SDK's host-test diskio) |
| SD picker, USB host (Launchpad / XL), USB audio, HostLink's USB | `src/emu_stubs.cpp` — inert |

## Build

    brew install sdl2        # once
    make -C emu              # emu/build/belt-emu

## Play

    emu/build/belt-emu                  # Mac mic in, default output out -- USE HEADPHONES
    emu/build/belt-emu --in voice.wav   # a 16-bit WAV, looped, instead of the mic
    emu/build/belt-emu --record out.wav # also record J9/J10

Knobs: drag up/down or scroll (double-click = centre). B1–B3: click and hold,
or hold the keys **1 2 3** (B2+B3 for 2 s opens Settings; B1 steps its pages).
CV: drag a jack's slider for volts, click GATE for 0 ↔ +5 V. The panel shows
the PLAY names, and the SETUP names while B3 is held; Settings pages are not
labelled (the firmware draws their rings as on the module).

## Test

    make -C emu test

Runs every `tests/*.emu` headless in real time and checks LEDs, output level
and pitch (Goertzel). Script language: top of `src/emu_script.cpp`.

- `tests/hold_freeze.emu` — HOLD by B2 (0.6 s), Freeze: B2 blue, the chord
  sustains in silence, singing C4 over it leaves E4 and no G4, release fades.
- `tests/hold_lock_j8.emu` — through the real Settings UI: Chord page Hold =
  Lock, J8 = Hold gate; the B2+B3 chord does not toggle HOLD; a J8 gate holds;
  Lock keeps E4 under a sung C4 and ducks in silence; gate low follows again.

## Not modelled

The M7's speed (the emulator's CPU meter is the Mac's — use the module for
CPU), the SD picker / firmware switching, Launchpad / Launch Control XL /
gamepad (next: CoreMIDI), USB audio mode, HostLink.
