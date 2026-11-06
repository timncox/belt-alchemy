# Pulse

A master clock and eight clocked CV outputs for the Hermetic Modular
Alchemy Lab V2: per-output ratio (/64 to x32), nine waveforms, width, level,
offset, phase, euclidean patterns, chance, swing and slew; tap tempo or an
external clock on J1, reset or start/stop on J2.

**Status: v0.1.0 builds and its engine passes 82 native tests. It has not
run on a module.** `DESIGN.md` has the panel map and what is verified.

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
