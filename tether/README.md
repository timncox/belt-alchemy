# TETHER

A second firmware for the Alchemy Lab V2: it turns USB MIDI from a keyboard
plugged into the Lab's USB port into pitch CV, gates and triggers. Made for
PROTECTOR's PARTS setting, which sends each part on its own channel
(1 KICK, 2 SNARE, 3 HATS, 4 PERC, 5 BASS, 6 LEAD, 7 PAD, 8 FX) plus clock,
start/stop and the section (CC 85 on channel 16).

## Plugging in

The Lab is the USB host and gives no power on its USB port. A keyboard
running on its own battery may still connect, but this has not been tried.
The safe set-up is a **powered USB-C hub** between the two: the hub powers and
charges the keyboard, and the Lab talks to it through the hub.

Hold **B1** while powering up for Mac mode (the port acts as a device again,
for the Mac tools; B3 shows blue). Mac mode is not remembered.

## Jacks (first boot)

| Jack | Job |
| --- | --- |
| J3 | GATE A (voice A = channel 5, BASS) |
| J4 | GATE B (voice B = channel 6, LEAD) |
| J5 | CLK 16 (a trig every sixteenth) |
| J6 | RESET (a trig on start) |
| J7 | TRIG 1 (KICK) |
| J8 | TRIG 2 (SNARE) |
| J9 | PITCH A, 1 V/oct, 0 V = middle C |
| J10 | PITCH B, 1 V/oct, 0 V = middle C |

J3 to J8 can each be given another job (JACKS, below): GATE A/B, CLK 24, CLK
16, CLK 8, CLK 4, BAR, RESET, RUN, SECTION (0 to 3 V for A to D), ACC A/B,
VEL A/B, TRIG 1 to 8.

## Controls

**VOICE** (no button held), for the voice B1 picks (B1 lit amber = A, cyan = B):

- P1 **PART**: channel 1 to 16, or ALL
- P2 **GATE**: GATE, TRIG, ACC GATE (8 V on an accent), VEL GATE (height = velocity)
- P3 **GLIDE**: 0 to 1 s
- P4 **OCTAVE**: -4 to +4
- P5 **TRANSPOSE**: -12 to +12 semitones
- P6 **NOTES**: LEGATO (slides keep the gate up), RETRIG, LOW, HIGH

**MASTER** (hold B2):

- P1 **CAL OFFSET**, P2 **CAL SCALE** for the voice B1 picks
- P3 **TRIG LENGTH** 1 to 50 ms, P4 **ACCENT** (velocity from 64 to 127)
- P5 **BEND** range, P6 **RESET ON** (start, or start and section)

**JACKS** (hold B3): P1 to P6 set the jobs of J3 to J8.

Buttons:

- B1 tap: the other voice
- B2 tap: all notes off
- B3 tap: a 300 ms test note on the voice
- B2 + B1: calibration. Each tap of B1 while B2 is held goes 1 V, 3 V, off on
  J9 and J10, with both gates up. Turn CAL OFFSET until a tuner reads the
  1 V note right, then CAL SCALE until the 3 V note is two octaves above it.
  Letting go of B2 ends it.

Lights: B2 flashes on the clock's beat (dim amber, no clock). B3 shows USB:
dim red nothing plugged in, amber connecting, red failed, green ready, a white
flash on each message.

Pot moves are saved after 5 s, as Pulse does.

## Building and testing

```
make                 # build/tether_alchemy.bin
make -C test         # the engine's own tests, on the computer
```

In the Lab emulator (alchemy-lab repo, `emu/`):

```
make test-custom FW_DIR=<path to>/tether
```

runs `emu-tests/*.emu`, which feed it MIDI with the emulator's `midi` command.
