# Belt on the Alchemy Lab — Design

Port of [schwung-belt](../belt) (Belt for the Ableton Move) onto the
Hermetic Modular **Alchemy Lab V2**, on the Alchemy SDK, in the shape of
[smack-alchemy](../smack-alchemy). Written 2026-09-10 before anything was
heard. Claims are marked **✅ verified** (with the source) or **⚠️ assumed /
unheard**; keep that discipline.

## 1. What carries over unchanged

- **The engine** (`src/vendor/belt_core.[ch]`, `plugin_api_v1.h`): since
  2026-10-02 the schwung-belt **`feat/hold` @ a86849c** copy (HOLD, below)
  on top of **`feat/chord-only` @ 1e25bfc**, which is
  PR #8's `feat/midi-harmony-v2` @ 3b44960 (played harmony, target mode,
  control notes, vel_sens; module 0.3.0, **open and unmerged**) plus the LEAD
  param. Not `main` (0.2.1): Hide and Seek needs all of it. Re-vendor from
  `main` once both merge. Before that: `main` @ e0eb4ad (0.2.0). ONE edit, in
  the header: `BELT_SR` 44100 → 48000, because libDaisy offers no 44.1 kHz.
  The engine converts YIN periods to Hz with that constant, so at 44100 a
  sung A would read 67.5 and the scale would land a semitone and a half
  sharp of the KEY pot. ✅ `test/test_tuner.c`: a 440 Hz sine reads
  note 69.00, C3 reads 48.00, hard-tune in C major pulls 449 Hz to 440.0.
  Consequences: the YIN lag ceiling puts the lowest trackable pitch at
  ~92 Hz rather than 85; the fixed latency is 24 ms rather than 26.
  Engine changes go upstream, never here.
- **The allocator** (`versio_alloc.[ch]`, from the Smack ports): the
  engine's five `calloc`s (~137 KB, ✅ test) land in a 1 MiB SDRAM pool.
- **48 kHz, 128-frame blocks.** The engine takes any block; 128 keeps the
  callback overhead where the family measured it.
- **Sample format**: float in/out, int16 interleaved into the engine, so
  a difference in sound is a shim bug, not a rewrite bug. The engine sums
  J1/J2 to mono for analysis and keeps the dry path stereo.
- **The picker, the linker script, the Makefile, `tools/hostlink-fs.mjs`**:
  copies of smack-alchemy's. A fix in one belongs in all three.

## 2. Control surface

Belt on the Move has 16 parameters on three knob pages plus pads. Here: six
pots on two pages (the SDK's Pager, second page held on B3 with pot catch),
three settings knobs, three buttons.

### PLAY page (base)

| Pot | Parameter | Engine | Ring |
|---|---|---|---|
| P1 | **KEY** C..B | `key` 0..11 | 12-zone selector **+ tuner** (§ LEDs) |
| P2 | **SCALE** Chromatic / Major / Minor / Harm Minor / Dorian / Mixolydian / Maj Pent / Min Pent / Blues | `scale` 0..8 | 9-zone selector |
| P3 | **RETUNE** 0–100, 0 = hard-tune | `retune` | level |
| P4 | **AMOUNT** 0–100 | `amount` | level |
| P5 | **HARMONY** level 0–100 | `harm_level` | level |
| P6 | **FORMANT** −100..+100, ±½ octave | `formant` | bipolar |

### SETUP page (hold B3)

| Pot | Parameter | Engine | Ring |
|---|---|---|---|
| P1–P4 | **VOICE 1–4** interval: Off / −Oct / −6th / −5th / −4th / −3rd / Unison / +3rd / +4th / +5th / +6th / +Oct | `harm1..4` 0..11 | 12-zone selector, one colour per voice |
| P5 | **DOUBLER** 0–100 | `double_amt` | level |
| P6 | **SPREAD** 0–100 | `spread` | level |

First boot after a flash: voice 1 = +3rd, voice 2 = +5th, voices 3–4 off,
no doubler, spread 70. The PLAY page adopts the physical pots at boot.

### Settings (B2+B3 held 2 s)

Page 0: brightness (SDK, P1), **FLEX** (P2), preset slot + action (SDK,
P3/P4), **HUMANIZE** (P5), **WET** (P6) — the three engine parameters
that did not fit the panel, each 0–100. Page 1 **FIRMWARE**: the SD picker
(smack-alchemy DESIGN.md § 4, unchanged).
Page 2 **CHORD** (2026-09-27): what held notes do.
- **P1 MIDI NOTES**: Off / Harmony (default) / Target (PR #8's
  `midi_mode`).
- **P2 LEAD** 0-100 (default 100): the corrected lead, the dry voice and
  the doubler together. 0 = chord only.
- **P3 HOLD**: Freeze (default) / Lock -- what HOLD does (below).
- **P4 J8**: Formant (default) / Hold gate.
- **P5 VEL SENS** (default 50).
- **P6 OCTAVE**: C2 / C3 (default) / C4 / C5 -- where the sequencer's chord
  roots start and what 0 V on a CV chord input is.

Page 3 **SOURCES** (2026-10-02, the last page the SDK allows:
`kSettingsMaxPages` = 4): where held notes come from besides the PLAY pads.
- **P1 CHORDS FROM**: Pads (default) / Seq / CV / MIDI. One at a time; a
  change releases every note the old source held.
- **P2 J3**: Hard gate (default) / Chord clock.
- **P3 TEMPO**: 40-240 BPM (default 120).
- **P4 CHORD**: Triads (default) / Sevenths.
- **P5 CV CAL**: Off / On (default) / Learn.
- **P6 CV INS**: J4 / J4-J5 / J4-J6 / J4-J7 (default).

Settings reopens on the page it was left on (the SDK keeps `page_`).

**The hands are always on.** The Launchpad PLAY pads and a USB-MIDI
keyboard (usb-keys: `keys_poll`) play whatever CHORDS FROM says -- the
selector picks the one extra source, the hands layer on top; changing the
source releases only that source's notes. `keys_poll` stops taking from
the keyboard's own 128-message queue once the note queue holds 32 of its
64, so a burst of keys never crowds out a chord change.

### Chord sources (2026-10-02, phase 2 of `docs/alchemy-chord-vocoder-design.md`)

Every source ends in `note_push()` → the audio callback's `belt_on_midi()`,
exactly as the PLAY pads do; nothing new runs in the audio callback except
J3's rising-edge counter. All of it runs in the 1 ms control poll
(`chords_poll`), also while Settings is open. The HAL-free rules are in
`src/chord_src.[ch]` (native test `test/test_chord_src.c`, ASan + UBSan).

- **Seq.** `src/vendor/seq.[ch]` is seq-alchemy `core/seq.[ch]`
  byte-identical (worktree-core b4453ea; unchanged through a4abb84 /
  chord-out ea93d84). Its transport and progression run on the control
  loop's millisecond clock (`System::GetUs()` wraps early on this SDK);
  the chord is `seq_chord_index()`. Root = Belt's KEY, scale = Belt's SCALE
  mapped to seq's (Chromatic → Major, Blues → Minor pentatonic; seq has
  neither), tones = Triads / Sevenths: tone *i* of a chord on degree *d* is
  scale degree *d* + 2*i* (seq.c's `note_for` for chord tones). The
  progression (count, degree and bars per chord) is saved in `BeltExtras`.
  Edited on the Launchpad CHORDS page (top 5; layout in the header of
  `belt_alchemy.cpp`), which also has run / stop (side 1). `seq_t` (~19 KB)
  is in SDRAM. A preset save pauses the poll, and the next poll catches up
  in one step -- a chord can change up to that pause late.
- **J3 = Chord clock.** Each rising edge (counted in `gate_poll_isr`, once
  per audio block -- 2.67 ms; a shorter trigger can be missed) moves the sequencer to its next
  chord; the tempo and bars are not used. With CV it samples the inputs.
  HARD is then B1 / the controllers only.
- **CV.** `hw.cv_jacks[1..4].Volts()` (the SDK's calibrated input, ~±10 mV
  in input mode), the learned offset taken off, smoothed (one-pole, ~5 ms),
  then a Schmitt trigger per input: a note moves once the pitch is 0.65
  semitone from it, to the nearest semitone, and has stayed 3 ms -- an input
  parked on a boundary never chatters, a sender off by up to the ±41.7 mV
  rounding margin still lands, and a jump commits once. The CV inputs in use
  are taken off the matrix (`Jack(n).Off()`) and given back to KEY / RETUNE
  / AMOUNT / HARMONY when CV stops being the source. **Calibration:** the
  sender plays the reference chord J4 0 V / J5 +4/12 / J6 +7/12 / J7 +1 V
  (C E G C); Learn averages 256 polls and stores measured − reference per
  input (mV, in `BeltExtras`); an offset over 0.25 V is a wrong patch and is
  refused. B3 shows the result for 3 s after Settings closes (green all,
  amber some, red none). This compensates the SENDER: the ±50 mV class of
  the MCP4728 jacks (J3-J6) is an output error on the other Lab.
- **MIDI.** `src/rear_midi.[ch]`: libDaisy `UartHandler` on USART1, RX PB7 =
  header pin 7 (`seed::D14`), 31,250 baud, circular DMA (DMA1_Stream5, which
  nothing else in this firmware or the SDK uses; neither touches USART1 --
  both checked by grep) into a 64-byte buffer in
  `.sram1_bss`, receive only (`Mode::RX`, no TX pin: header pin 8 is never
  driven); the interrupt only copies bytes into a 256-byte ring, and
  libDaisy's `MidiParser` reads them in the control loop. Not
  `MidiUartHandler`: it parses in the interrupt and its event FIFO is 256 ×
  ~140 B (sysex buffers) = ~36 KB of SRAM. Notes on any channel; note-on
  velocity 0 = off. Clock, start / stop, CCs: ignored in this version.
  Cable: pin 8 ↔ pin 7 crossed both ways plus grounds only -- pins 1 / 11 /
  15 are −12 V / +12 V / 3V3A; **never a straight ribbon**.
  ✅ `test/test_rear_link.cpp`: seq-alchemy chord-out's own
  `core/chord_midi.c` (ea93d84) sends C - Am - F - G - C - stop with clock
  (469 bytes, 389 × F8); through `rear_midi.cpp` + libDaisy `MidiParser` +
  the engine, Belt holds exactly the sender's notes every millisecond, the
  engine's held count agrees, 13 on / 13 off, nothing after stop.

✅ emulator (alchemy-lab `emu-chords`, `make test-belt-chords`): every
source, plus every tests/belt script against this build. ⚠️ Nothing on
hardware: USART1 electrically, real CV accuracy, CPU of the extra poll work.

**Hide and Seek** = MIDI NOTES Harmony, LEAD 0, Setup's Voice 1-4 intervals
Off, then hold a chord and sing. Each held note is the singer's own voice
re-pitched to it; there are four voices, and the newest press steals the
oldest note's voice. ✅ `test/test_chord.c` at 48 kHz, voice-like A3 with
C4-E4-G4 held: C4 23.4, E4 15.3, G4 18.8 (Goertzel power), D#4 0.05, and the
A3 lead drops from 25.5 to 0.002 at LEAD 0. Release gives silence.

### HOLD (2026-10-02)

The harmony voices keep the notes they are on while the singer carries on.
Engine params `hold` (performance, never in the state blob) and `hold_mode`
(saved; CC 39). On engage every sounding harmony voice snapshots its note --
its interval from the current target, or the played note it was pinned to.

- **Freeze**: the 4096 samples ending at the last voiced analysis (a HOLD
  hit in a breath freezes the note just sung) become the source; grains read
  it at random marks at the held notes, so the chord sustains as a pad
  through silence and ignores what is sung next. Release fades ~1/3 s, then
  the live voices return. +16 KB in `belt_t`; the pool went 160 -> 176 KB.
- **Lock**: the live voice stays the source: the words on the held chord,
  ducking in the gaps like normal harmonies.

Triggers, all toggles of one latch, ORed with the J8 gate: B2 held 0.6 s,
Launchpad top 4, Launch Control XL upper 3, gamepad L3 (was Stutter 1/2).
J8 in Hold-gate mode holds while high (`cv_matrix.Jack(5).Custom`, magnitude
thresholds like J3). B2 is blue while held. Status field 7: 0 off, 1 locked,
2 frozen, 3 fading. ✅ upstream sim tests 27-32; ⚠️ unheard on hardware.

### Launchpad: KEY and PLAY (top button 3)

KEY (default, as shipped) is described in the header of `belt_alchemy.cpp`.

PLAY turns rows 1-6 into three octaves, two rows each, sharps over
naturals:
- rows 1-2: C5-B5
- rows 3-4: C4-B4
- rows 5-6: C3-B3

Held pads are green, the root blue, in-scale notes dim blue. Rows 7-8 stay
the voices and the tuner.

- **Pads send velocity 100.** The Mini MK3 has none.
- **Pad notes are queued, not sent directly.** They go through a
  single-producer queue drained at the top of the audio callback, because
  the engine reads its held-note table inside `belt_process`.
- **Releases always go through**, even with Settings open, and leaving PLAY
  releases every held pad, so a note cannot stick.
- **PLAY/KEY is not saved.** Every boot starts in KEY.

### Buttons

| Button | Gesture | Does |
|---|---|---|
| **B1 HARD** | hold | hard-tune while held: instant, full correction (`hard` = 1) |
| | tap | latch hard-tune on / off |
| **B2 HARMONIES** | tap | latch the harmony mute (`harm_level` 0, pot value restored after) |
| | hold 0.6 s | HOLD on / off (fires at 0.6 s; the release does nothing). B2 no longer mutes while held -- the Launchpad / XL MUTE buttons keep that momentary |
| **B3 SETUP** | hold | the SETUP page |
| B2 + B3 | hold 2 s | Settings (SDK). B2 stands down while B3 is held, so the chord neither mutes nor latches |

B1 (and the Launchpad / XL HARD and MUTE buttons): effective state = latch
XOR held, a tap (released within 300 ms) flips the latch. J3 ORs into HARD.

### LEDs

| Where | Shows |
|---|---|
| KEY ring | the SDK's 12-zone selector, plus the **tuner**: in-scale notes as dim dots, the sung note as a green pip within ±25 cents of its target, orange outside |
| other rings | the SDK draws every pot's value; SETUP rings while B3 is held |
| B1 pair | white while hard-tune is on; else the tuner colour (green / orange / dim blue unvoiced) |
| B2 pair | purple while any voice has an interval and is not muted; **blue while HOLD is on** (dim if muted); grey muted; dim none; **red = callback over 80%** |
| B3 pair | dim Setup tint |
| P1 ring, first 2.5 s | last session's worst CPU load: dim blue = no data; green < 75%, amber < 90%, red = it did not fit |

The tuner reads the engine's `status` param once per frame
(`note10:cents:voiced:mask`). The KEY ring's zone geometry copies the SDK's
Distributed selector (zone *i* of 12 at LED `round(i·12/11)`), so the
tuner pip lands on the zone the pot would select for that note. ⚠️ Unseen.

### CV (J3–J8 = CV 0–5)

| Jack | Role |
|---|---|
| J3 | **HARD gate** — raw ADC in the callback, +1.5 V assert / +0.5 V release on the magnitude of the deviation from the calibrated 0 V code (smack-alchemy's clock-jack reader); or the **chord clock** (Settings, Sources, P2) |
| J4 | → KEY (12 zones across the CV range; not V/oct), or chord pitch 1 (Sources: CV) |
| J5 | → RETUNE, or chord pitch 2 |
| J6 | → AMOUNT, or chord pitch 3 |
| J7 | → HARMONY, or chord pitch 4 |
| J8 | → FORMANT, or the **HOLD gate** (Settings, Chord page, P4) |

V/oct into the harmony target needs the engine's Target mode, which is in
schwung-belt PR #8 and not yet on `main`; when it lands, J4 → held note is
the obvious v0.2.

## 3. Persistence

The SDK's **Presets** manage the pager (both pages), Settings (brightness,
flex, humanize, wet; midi notes, lead, vel sens; the Sources page) and
`BeltExtras` {`cpu_peak`, `cpu_avg`, the chord progression, the CV offsets}.
The chord-sources build changed both the Settings schema (Octave + the
Sources page) and `BeltExtras` (schema 0x03), so its first boot resets
Belt's saved state once.
Adding the Chord page changed the Settings schema hash, so the first boot of
the `hide-and-seek` build finds no valid Belt state and starts from Belt's
first-boot defaults, once. Slot 0 is the working
state, **autosaved** 5 s after the last SETUP / Settings / extras change
with hands off the buttons and the picker idle. `BeltExtras` carries its
own schema tag ('BLT'), so a slot 0 written by Smack or Mark on the same
module fails the slot's schema gate (✅ `presets.cpp`: XOR of every managed
component) and lands in first-boot defaults rather than being read as
Belt's.

## 4. Budgets (✅ from the 2026-09-10 build)

| Region | Used | Of |
|---|---|---|
| SRAM (code + data, BOOT_SRAM) | 320,128 B | 480 KB (65 %) |
| DTCM | 74,496 B | 128 KB |
| SDRAM | 1.1 MB | 64 MB (the 1 MiB pool + HostLink) |
| `.bin` | 312,196 B | |

CPU: ⚠️ **unmeasured, and the open question of this port.** On a Mac the
engine ran 65× realtime with all seven voices; the Move's A53 was estimated
at 10–15 %. A 480 MHz Cortex-M7 is several times slower per core than that
A53 on this kind of float DSP, so the first rack power-on should watch the
B2 alarm and the boot bar. If it does not fit: `-ffast-math` on the engine
unit, then fewer voices by default, then the rings into AXI SRAM.

## 5. Open questions

1. ⚠️ Does anything play? First rack / Erica-case power-on answers it.
2. ⚠️ CPU with seven voices (§ 4).
3. ⚠️ Gate polarity on J3 (magnitude-thresholded, so it should not matter).
4. ⚠️ The tuner pip's geometry against the SDK's selector on real LEDs.
5. The SDK is beta; the pinned SHAs are what this was built against.
