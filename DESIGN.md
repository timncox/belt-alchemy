# Belt on the Alchemy Lab — Design

Port of [schwung-belt](../belt) (Belt for the Ableton Move) onto the
Hermetic Modular **Alchemy Lab V2**, on the Alchemy SDK, in the shape of
[smack-alchemy](../smack-alchemy). Written 2026-09-10 before anything was
heard. Claims are marked **✅ verified** (with the source) or **⚠️ assumed /
unheard**; keep that discipline.

## 1. What carries over unchanged

- **The engine** (`src/vendor/belt_core.[ch]`, `plugin_api_v1.h`): since
  2026-09-27 the schwung-belt **`feat/chord-only` @ 1e25bfc** copy, which is
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
- **P5 VEL SENS** (default 50).

The Launchpad's PLAY pads (below) are the note source for now. The chord
sequencer and the front/back links of `docs/alchemy-chord-vocoder-design.md`
feed the same `belt_on_midi`.

**Hide and Seek** = MIDI NOTES Harmony, LEAD 0, Setup's Voice 1-4 intervals
Off, then hold a chord and sing. Each held note is the singer's own voice
re-pitched to it; there are four voices, and the newest press steals the
oldest note's voice. ✅ `test/test_chord.c` at 48 kHz, voice-like A3 with
C4-E4-G4 held: C4 23.4, E4 15.3, G4 18.8 (Goertzel power), D#4 0.05, and the
A3 lead drops from 25.5 to 0.002 at LEAD 0. Release gives silence.

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
| **B2 HARMONIES** | hold | mute the harmony voices while held (`harm_level` 0, pot value restored after) |
| | tap | latch the mute |
| **B3 SETUP** | hold | the SETUP page |
| B2 + B3 | hold 2 s | Settings (SDK). B2 stands down while B3 is held, so the chord neither mutes nor latches |

Both use one gesture: effective state = latch XOR held, a tap (released
within 300 ms) flips the latch. J3 ORs into HARD.

### LEDs

| Where | Shows |
|---|---|
| KEY ring | the SDK's 12-zone selector, plus the **tuner**: in-scale notes as dim dots, the sung note as a green pip within ±25 cents of its target, orange outside |
| other rings | the SDK draws every pot's value; SETUP rings while B3 is held |
| B1 pair | white while hard-tune is on; else the tuner colour (green / orange / dim blue unvoiced) |
| B2 pair | purple while any voice has an interval and is not muted; grey muted; dim none; **red = callback over 80%** |
| B3 pair | dim Setup tint |
| P1 ring, first 2.5 s | last session's worst CPU load: dim blue = no data; green < 75%, amber < 90%, red = it did not fit |

The tuner reads the engine's `status` param once per frame
(`note10:cents:voiced:mask`). The KEY ring's zone geometry copies the SDK's
Distributed selector (zone *i* of 12 at LED `round(i·12/11)`), so the
tuner pip lands on the zone the pot would select for that note. ⚠️ Unseen.

### CV (J3–J8 = CV 0–5)

| Jack | Role |
|---|---|
| J3 | **HARD gate** — raw ADC in the callback, +1.5 V assert / +0.5 V release on the magnitude of the deviation from the calibrated 0 V code (smack-alchemy's clock-jack reader) |
| J4 | → KEY (12 zones across the CV range; not V/oct) |
| J5 | → RETUNE |
| J6 | → AMOUNT |
| J7 | → HARMONY |
| J8 | → FORMANT |

V/oct into the harmony target needs the engine's Target mode, which is in
schwung-belt PR #8 and not yet on `main`; when it lands, J4 → held note is
the obvious v0.2.

## 3. Persistence

The SDK's **Presets** manage the pager (both pages), Settings (brightness,
flex, humanize, wet; midi notes, lead, vel sens) and `BeltExtras` {`cpu_peak`}.
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
