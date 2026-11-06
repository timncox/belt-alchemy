# Pulse

A master clock and eight clocked CV outputs for the Hermetic Modular
Alchemy Lab V2: per-output ratio (/64 to x32), nine waveforms, width, level,
offset, phase, euclidean patterns, chance, swing and slew; tap tempo or an
external clock on J1, reset or start/stop on J2; J7/J8 can be switched to
two CV inputs that move any setting of any output.

**Status: v0.2.1 builds, its engine passes 100 native tests and it passes 60
checks in the Alchemy Lab emulator. It has not
run on a module.** `DESIGN.md` has the panel map and what is verified. The manual is
`docs/site.html`, served at https://timncox.github.io/alchemy-lab/pulse/.

## Build

From the repo root, once: `make libdaisy`. Then:

```sh
cd pulse
make          # build/pulse_alchemy.bin
make test     # native engine tests, no hardware
```

## Flash

Same as Belt: copy `build/pulse_alchemy.bin` into the card's `alchemy`
folder and pick it on the firmware page (B2+B3 for two seconds, B1 to the
Firmware page), or `make program-dfu` / the web programmer.

## Emulator

With [the Alchemy Lab emulator](https://github.com/timncox/alchemy-lab/tree/emu-custom/emu)
checked out (branch `emu-custom`; macOS needs `brew install sdl2`):

```sh
cd alchemy-lab/emu
make test-custom FW_DIR=/path/to/belt-alchemy/pulse   # smoke + emu-tests/*.emu
make FW=custom FW_DIR=/path/to/belt-alchemy/pulse && build/custom/pulse_alchemy-*/emu
```

`emu.mk` tells the emulator how to build Pulse; `emu-tests/` has the scripts.
