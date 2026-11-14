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

Page 3 **FOLLOW** (2026-11-14): chords that follow the sung note
(`src/belt_follow.c`, below).
- **P1 FOLLOW**: Off (default) / Root / Soprano / Alto / Tenor / Bass /
  Creative.
- **P2 STICK** 0-100 (default 40): how long a new note must last before the
  chord changes, 20 ms + 580 ms x (STICK/100)^2 (about 110 ms at 40).
- **P3 SIZE**: Triad (default) / 7th / 9th.
- **P4 BASS**: Off / On (default).
- **P5 COLOR** 0-100 (default 50): Creative only.

The Launchpad's PLAY pads (below) are the note source for now. The chord
sequencer and the front/back links of `docs/alchemy-chord-vocoder-design.md`
feed the same `belt_on_midi`.

**Hide and Seek** = MIDI NOTES Harmony, LEAD 0, Setup's Voice 1-4 intervals
Off, then hold a chord and sing. Each held note is the singer's own voice
re-pitched to it; there are four voices, and the newest press steals the
oldest note's voice. ✅ `test/test_chord.c` at 48 kHz, voice-like A3 with
C4-E4-G4 held: C4 23.4, E4 15.3, G4 18.8 (Goertzel power), D#4 0.05, and the
A3 lead drops from 25.5 to 0.002 at LEAD 0. Release gives silence.

### FOLLOW (2026-11-14)

With FOLLOW on, the four harmony voices stop copying every sung note at
fixed intervals: they hold a chord in KEY + SCALE built on the sung note and
change it only when a new quantized note has lasted STICK. A phrase's first
note (nothing yet, or 0.4 s of silence before it) commits after 25 ms, so the
chord is there with the voice; after a breath the voicing is built fresh
around the new note rather than led from where the last phrase ended.
Changing a FOLLOW setting, KEY or SCALE rebuilds the current chord at once.
Engine params `follow`, `follow_stick`, `follow_size`, `follow_bass`,
`follow_color` (CC 40-44, appended); `get_param("chord")` reads it back.

- **Root**: the sung note is the root of the scale's chord on it (major when
  the note is outside the scale): 3rd and 5th above, root an octave below,
  and the 5th below (triad) / 7th / 9th above.
- **Soprano / Alto / Tenor / Bass**: the sung note is that part. Candidates
  are the scale's diatonic chords that hold the note (Pentatonic borrows its
  parent major/minor, Blues natural minor, Chromatic major; outside the
  scale, major/minor chords weighted toward secondary dominants). Scored by
  degree (I, IV, V first), staying put, and root motion by 4ths/5ths; then
  every voicing in range is scored for chord-tone coverage (no doubled 3rd,
  root in the bass), smallest motion from the last voicing, no parallel 5ths
  or octaves, nothing below A1 and a cost below E2. BASS Off drops the bass
  part (three parts).
- **Creative**: the sung note sits on top as a colour tone (9th, maj7/7th,
  6th, 11th, #11) over an open voicing with the root in the bass; COLOR
  sets how far out the chord choice reaches.

HOLD snapshots the followed chord like any other; held MIDI notes still
take over the voices they are pinned to. The Setup page's VOICE 1-4
intervals are unused while FOLLOW is on. A commit costs one voicing search
(a few thousand candidate scores), at most a few times a second.

✅ `test/test_follow.c` at 48 kHz with a synthesized singer (buzz through
three vowel formants, 5.5 Hz vibrato, a 70-cent scoop into every note,
consonant gaps and breaths): Root voices measured at their pitches; Soprano
/ Alto / Tenor / Bass / Creative over a tune in C major keep the sung note
in the chord with at most one leap over a 5th in any part; a 60 ms passing
note does not change the chord at the default STICK; 70 ms gaps never
flicker it; HOLD keeps the C chord while D is sung. Chord change 56 ms after
the note at STICK 0, 152 ms at 40, 427 ms at 80; 61 ms for a phrase's first
note. Host cost +5-7 us per 128-frame block (Creative, worst case).
✅ Emulator (`alchemy-lab/emu`, belt-phone build, `test/emu/follow.emu`):
Root C4 -> G4 E4 G3 C3 and F4 -> A4 F3, HOLD keeps G4 while D4 is sung,
Soprano and Creative chords at their pitches.

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
| J3 | **HARD gate** — raw ADC in the callback, +1.5 V assert / +0.5 V release on the magnitude of the deviation from the calibrated 0 V code (smack-alchemy's clock-jack reader) |
| J4 | → KEY (12 zones across the CV range; not V/oct) |
| J5 | → RETUNE |
| J6 | → AMOUNT |
| J7 | → HARMONY |
| J8 | → FORMANT, or the **HOLD gate** (Settings, Chord page, P4) |

V/oct into the harmony target needs the engine's Target mode, which is in
schwung-belt PR #8 and not yet on `main`; when it lands, J4 → held note is
the obvious v0.2.

## 3. Persistence

The SDK's **Presets** manage the pager (both pages), Settings (brightness,
flex, humanize, wet; midi notes, lead, vel sens) and `BeltExtras` {`cpu_peak`}.
Adding the Chord page changed the Settings schema hash, so the first boot of
the `hide-and-seek` build finds no valid Belt state and starts from Belt's
first-boot defaults, once. Adding the Follow page (2026-11-14) does the same
once more; the USB mode lives in `/alchemy/usb.cfg` on the card and is kept. Slot 0 is the working
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
