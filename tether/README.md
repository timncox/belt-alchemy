# TETHER

A second firmware for the Alchemy Lab V2: it turns USB MIDI from a keyboard
plugged into the Lab's USB port, or from a computer, iPhone or iPad the Lab
is plugged into, into pitch CV, gates and triggers. From 0.5.0 the Lab is a
USB audio device too: the FM-1 (or a computer) can play audio out of J9 and
J10 and hear J1 and J2. Made for
PROTECTOR's PARTS setting, which sends each part on its own channel
(1 KICK, 2 SNARE, 3 HATS, 4 PERC, 5 BASS, 6 LEAD, 7 PAD, 8 FX) plus clock,
start/stop and the section (CC 85 on channel 16).

## Plugging in

The front USB-C works both ways round (0.2.0), and finds out which by itself:

- **A keyboard** (FM-1, SMK-37, any class-compliant USB-MIDI device) plugged
  straight into the Lab: the Lab is the host. B3 goes green.
- **A computer, iPhone or iPad**: the Lab shows up as a USB-MIDI device named
  **TETHER**, so an app or a DAW can play it. B3 goes cyan.

The port takes turns being a host (1.5 s) and a device (2.5 s) until one of
them answers, then stays that way until it's unplugged. So a keyboard or a
computer is picked up within about 4 s of plugging in.

The Lab puts no power on its USB port (hardware: no VBUS switch exists, so
firmware can't add it). The FM-1 and SMK-37 run on their own power, but their
USB engine only starts with 5 V on the cable (fm1-base and smk37-base usb.c:
"Powered on without a cable, the SIE does not answer"). Straight in, the Lab
sees the pull-up but the port never comes out of the bus reset (0.3.3 log:
WAIT_FOR_ATTACHMENT, PortEnabled never set, abort every ~7 s; B3 red, 1
blink). So they need a powered hub (or a USB-C adapter that adds 5 V) between
them and the Lab, which also charges them. Checked on hardware Nov 22: direct
fails, powered hub works.

**PROTECTOR on the FM-1 or the SMK-37:** set CLOCK OUT to **PARTS** (SONG
page) so every part comes over USB on its own channel with the clock, start
and stop. The SMK-37 (PROTECTOR 0.4.7, USB name "Felucca") is audio control
0, MIDI 1, audio in 2, so the host opens its MIDI as it is (the FM-1 needs
its MIDI moved, 0.1.1). Use the powered hub.

Hold **B1** while powering up for Mac mode (the port is the Mac tools' device,
for HostLink; B3 shows blue). Mac mode is not remembered.

## Jacks (layout BAND, the default from 0.3.0)

| Jack | Job |
| --- | --- |
| J1 | in: a clock when no MIDI clock comes (MORE P5: 1 to 48 PPQN, 4 at first; or another job, below) |
| J2 | in: RESET (MORE P6; or another job, below) |
| J3 | GATE A (voice A = channel 5, BASS) |
| J4 | GATE B (voice B = channel 6, LEAD) |
| J5 | TRIG 3 (HATS) |
| J6 | CLK 16 (a pulse every sixteenth) |
| J7 | TRIG 1 (KICK) |
| J8 | TRIG 2 (SNARE) |
| J9 | PITCH A, 1 V/oct, 0 V = middle C (24-bit) |
| J10 | PITCH B, 1 V/oct, 0 V = middle C (24-bit) |

Layout **CLOCK** is 0.1 and 0.2's: J3 GATE A, J4 GATE B, J5 CLK 16, J6 RESET,
J7 KICK, J8 SNARE, J9 and J10 pitch A and B.

Layout **CV IN** (0.4.0) keeps BAND's gates, drums and pitches and turns two
jacks into inputs: J5 **IN FILTER** and J6 **IN DELAY**.

Layout **AUDIO** (0.5.0) is CV IN with J9 **AUDIO L** and J10 **AUDIO R**: the
USB audio from the FM-1 or a computer, left and right (below).

Change any jack and the layout reads CUSTOM.

### J1 and J2's jobs (0.5.2)

J1 and J2 are inputs only (the codec's, AC-coupled: they hear pulses and
audio, not steady voltages). Each takes one job, on MORE (hold B1): P5 for
J1, P6 for J2. Turning the pot clockwise:

| Job | J1 | J2 | What a rising edge or a sound does |
| --- | --- | --- | --- |
| **CLOCK** 1, 2, 4, 8, 12, 24, 48 | yes (4 at first) | no | the clock, at that many pulses a beat, when no MIDI clock comes |
| **RESET** | yes | yes (at first) | back to the top of the bar |
| **START/STOP** | yes | yes | starts and stops the transport (a CLOCK on J1 then only counts while running) |
| **NEXT SECTION** | yes | yes | PROTECTOR goes to its next section (at the next bar while playing): CC 86 127, channel 16, over the front USB-C. Needs PROTECTOR 0.5.1 |
| **LOUD** FILTER .. MORPH | yes | yes | how loud the jack is steers that effect of PROTECTOR's, as an IN job does on J3 to J8 (CC 102 to 115): silence leaves the knob alone, about 1.25 V peak is all the way |
| **OFF** | yes | yes | nothing |

CLOCK, RESET and START/STOP wait while USB MIDI clock comes in (MIDI wins, as
before), so with the FM-1 on LAB (which sends its clock) they only matter
with the Lab on its own. NEXT SECTION and LOUD work either way. Whatever the
job, J1 and J2 still go back to the FM-1 or the computer as USB audio.
Settings saved before 0.5.2 load J1 as CLOCK and J2 as they were.

## USB audio (0.5.0): the FM-1's sound on J9 and J10, J1 and J2 back

While the Lab is a USB device (a computer, an iPhone, or the FM-1 running
the link; B3 cyan), it is a class-compliant USB audio interface as well as
the TETHER MIDI device, in one configuration (USB audio class 1.0, no IAD):

- **USB to the Lab** (interface 1, iso OUT 0x02, adaptive): stereo, 16 bit,
  44.1 kHz. A jack set to **AUDIO L** or **AUDIO R** (J9 and J10 only; pink on
  the ring) plays it at ±5 V for full scale. Nothing streamed: silence.
- **The Lab to USB** (interface 2, iso IN 0x83, asynchronous): J1 left, J2
  right, the same format, whenever the host listens. J1 and J2 keep their
  clock and reset jobs at the same time.
- **MIDI** is interface 3 (it was 1), otherwise as before: CC out from the CV
  inputs, SysEx (Loadout), clock and notes in.

The Lab runs at 48 kHz, USB at 44.1 kHz. Coming in, a cubic resampler reads
the incoming stream a little faster or slower to keep its buffer about 3 ms
full, so it follows the sender's clock (the FM-1 sends at its I2S rate,
about 44,117.6 Hz: +400 ppm, tested to ±2000 ppm). Going out, the Lab sends
44 or 45 frames a millisecond, 44.1 on average by its own clock, one more or
one fewer as its buffer drifts, and the host follows it (asynchronous: no
feedback endpoint). A stream starts and stops with a 5 ms fade, so there's no
click; a gap holds the last sample and fades.

For the curious: the device is now class 0 (each interface says what it is;
libDaisy's MIDI mode said CDC), release 5.00, same vendor, product and name.
The configuration is 249 bytes. The OTG FIFO RAM is laid out again for the
192-byte packets (RX 2 KB, EP0 256 B, MIDI in 512 B, audio in 512 B, of 4 KB).
TETHER serves this with its own USB class (`src/tether_usbd.cpp`) on
libDaisy's device core; Mac mode's HostLink is untouched.

## CV in (0.4.0): the Lab's jacks steer PROTECTOR's effects

J3 to J8 can each be set to an input job, IN FILTER, IN REVERB, IN DELAY, IN
ECHO, IN GLITCH, IN PUMP, IN RUMBLE, IN DRIVE, IN COMB, IN PHASER, IN FLANGER,
IN CRUSH, IN SCENE or IN MORPH (teal on the ring). The jack then reads a
voltage and sends it out of the front USB-C as the CC PROTECTOR's effects pad
already plays (CC 102 to 115, channel 16), to whatever has the Lab as its
USB-MIDI device: the FM-1 with CLOCK OUT on **LAB**, or a computer. PROTECTOR
needs no change.

- IN FILTER: 0 V is the middle (filter off), -5 V closes it, +5 V opens it.
- Every other one: 0 V is none, +5 V is all the way.
- A jack sends nothing until it first reads more than 0.2 V, so a CV jack with
  nothing in it leaves PROTECTOR's knob alone.
- Setting a jack back to an output sends CC 116 0, which hands every effect
  back to its knob (the phone's pad too).
- J9 and J10 are outputs only (or USB audio, 0.5.0).

J1 and J2 are the codec's inputs and can't be outputs. MIDI clock wins: while
USB MIDI clock is coming (a tick in the last half second), J1 and J2 are
ignored. J1 clocks on rising edges above about 1 V. In RESET mode the first
J1 pulse starts the transport, and three of J1's periods without a pulse stop it.

Every output tops out at about 5 V (the SDK: all outputs are ±5 V at the
panel), so none reaches the 8 to 10 V some drum modules want for their loudest.

Jobs for J3 to J10: PITCH A/B/C, GATE A/B/C, ENV A/B/C (the voice's ADSR, 0
to 5 V), ACC A/B/C, VEL A/B/C, CLK 24, CLK 16, CLK 8, CLK 4, BAR, RESET, RUN,
SECTION (0 to 3 V for A to D), TRIG 1 to 8, for J3 to J8 only the 14
CV inputs (IN FILTER to IN MORPH, above), and for J9 and J10 only AUDIO L and
AUDIO R (USB audio).

**Drums (TRIG 1 to 8, MIDI channels 1 to 8).** Each drum has a MODE:

- **GATE** or **GATE + VEL** (the default, with VEL on for KICK, SNARE, HATS):
  up from note on to note off, at least TRIG LENGTH, so a module whose sound
  follows the gate's length hears it.
- **TRIG** or **TRIG + VEL**: TRIG LENGTH, whatever the note's length.
- With **VEL**, the height follows velocity, from FLOOR (velocity 1; 2.5 V at
  first, which still fires a 2.5 V trigger input) to 5 V (velocity 127).
  Without it, 5 V.

A hit that lands while the last is still up, or the moment it ends, drops the
jack for 3 ms first so the module hears two hits. J3 to J6 update about every
millisecond, J7 and J8 within one; so a drum on J3 to J6 can be up to about
1 ms later than one on J7 or J8, and a hit that starts as the last one ends
up to about 3 ms later still.

J3 to J6 are written from the main loop, which stops for a few milliseconds
each frame while the screen and lights are drawn. So that no hit or clock
pulse is lost in a stop, every rise is counted as it happens, and the jack
then shows a low and the hit, each for 3 ms at least, late if it must be
(gates, triggers, clocks, accents, reset, run; never pitch, envelopes,
velocity or section). Before this, about one hat in twenty on J5 could run
into the last one in the emulator.

## Controls

**VOICE** (no button held), for the voice B1 picks (B1 amber = A, cyan = B,
purple = C):

- P1 **PART**: channel 1 to 16, or ALL
- P2 **GATE**: GATE, TRIG, ACC GATE (5 V on an accent, 3.5 V otherwise), VEL GATE (1 to 5 V)
- P3 **GLIDE**: 0 to 1 s
- P4 **OCTAVE**: -4 to +4
- P5 **TRANSPOSE**: -12 to +12 semitones
- P6 **NOTES**: LEGATO (slides keep the gate up), RETRIG, LOW, HIGH

Voices A, B and C follow channels 5, 6 and 7 (BASS, LEAD, PAD) at first.

**MORE** (hold B1), for the same voice: P1 **ATTACK**, P2 **DECAY**, P3
**SUSTAIN**, P4 **RELEASE** (1 ms to 4 s), P5 **J1**'s job (CLOCK at 1, 2,
4, 8, 12, 24 or 48 pulses a beat, RESET, START/STOP, NEXT SECTION, LOUD into
one of 14 effects, OFF), P6 **J2**'s job (the same without CLOCK); above.

**MASTER** (hold B2):

- P1 **CAL OFFSET**, P2 **CAL SCALE** for the voice B1 picks
- P3 **TRIG LENGTH** 1 to 50 ms, P4 **ACCENT** (velocity from 64 to 127)
- P5 **BEND** range, P6 **RESET ON** (start, or start and section)

**JACKS** (hold B3): P1 to P6 set J3 to J8. Tap B1 while B3 is held for page
2: P1 J9, P2 J10, P3 **LAYOUT** (BAND, CLOCK, CV IN, AUDIO, CUSTOM), P4 **DRUM** (which of
TRIG 1 to 8 the next two set), P5 **MODE** (GATE, GATE + VEL, TRIG, TRIG +
VEL), P6 **FLOOR** (0 to 5 V).

Buttons:

- B1 tap: the next voice
- B2 tap: all notes off
- B3 tap: a 300 ms test note on the voice
- B2 + B1: calibration. Each tap of B1 while B2 is held goes 1 V, 3 V, off on
  J9 and J10, with both gates up. Turn CAL OFFSET until a tuner reads the
  1 V note right, then CAL SCALE until the 3 V note is two octaves above it.
  Letting go of B2 ends it.

Lights: B2 flashes on the clock's beat (dim amber, no clock). B3 shows USB:
dim red nothing plugged in, amber connecting, red failed, green a keyboard,
cyan a computer, iPhone or iPad, a white flash on each message.

A keyboard straight on the port that won't start (0.3.1, 0.3.2): TETHER keeps
trying, each time from scratch, and after three tries in a row B3 goes red and
blinks dark every 3 s to say where it stopped:

- **1 blink:** the keyboard never answered. Its USB isn't running; for the
  FM-1 and SMK-37 that means no 5 V on the cable (use the powered hub).
- **2 blinks:** it answered, then was lost taking its address or giving its
  descriptors.
- **3 blinks:** it gave its descriptors, but didn't take SET_CONFIGURATION.
  This was the FM-1 on 0.3.1: it lists a serial number it can't give, stalls
  the request for it, and then loses the next one. 0.3.2 doesn't ask a device
  straight on the port for its names (the hub path never did).
- **4 blinks:** it's set up, but no MIDI was found or it wouldn't open (a mouse,
  say).

Each failed try (and each keyboard that starts) also writes a log to the card
as `/alchemy/tether_usb.txt`, at most every 3 s: the state at that moment
(the device and its configuration bytes), then the last 128 steps, with the
cable coming and going and the port's turns as host and device. (0.3.2 kept
the log where a turn to device mode wiped it, and could write an empty one;
0.3.3 keeps it for the whole power-up and writes only after a try.) Read it in Mac mode with HostLink
(`node tools/hostlink-fs.mjs get /alchemy/tether_usb.txt tether_usb.txt`).

It keeps trying while red, and goes green as soon as one works.

## The iPhone app and other firmware (0.5.1)

Plug the iPhone (or iPad) into the Lab's front USB-C where the FM-1 goes,
open the Alchemy Lab app and tap **TETHER** in its device list once (the app
remembers it). The app then shows the panel's lights live, presses B1, B2
and B3 for you (hold B3 on the phone for the jack page, as on the panel) and
switches the Lab to another firmware from the card. Loadout's settings keep
working on the same cable. The FM-1 and the iPhone share the one port, so
it's one or the other at a time. Nothing to set: Mac mode (B1 at power-up)
is unchanged.

The Firmware page's file knob also shows folders inside /alchemy (one
level): a purple dot opens the folder with the same flash gesture, and
inside, the first (white) dot goes back up.

## Settings from Loadout (SysEx)

With the Lab plugged into an iPhone, iPad or Mac as the TETHER USB-MIDI
device, an app can read and set every setting with SysEx (`src/tether_sysex.h`):
`F0 7D 54 45 01 F7` asks for them, `02` + the 80-byte state as 160 nibbles sets
them all, `03 n` picks layout n (0 BAND, 1 CLOCK, 2 CV IN, 3 AUDIO).
Settings saved by 0.3.3 and 0.4.0 load unchanged in 0.5.0 (the state's layout
version is still 2); 0.4.0 would read AUDIO L / R as PITCH A / B. TETHER answers `10` + its
version + the state, or `11` + why not.

Settings saved by 0.1 or 0.2 don't carry over: 0.3.0 starts from its defaults.

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

runs `emu-tests/*.emu`, which feed it MIDI with the emulator's `midi` command
and (0.5.0, `usbaudio.emu`) USB audio with its `usbaudio` command: the
emulator's far end streams a sine at 44.1 kHz by its own clock and listens to
J1 / J2, through the same rings and resamplers as the module. The module's USB
stack itself (descriptors on the wire, iso timing) is not emulated;
`test/test_usb_audio.cpp` checks the descriptors byte by byte and the
resamplers and packet sizer against drifting clocks.
