# Pulse on the Alchemy Lab — Design

A master clock and eight clocked CV outputs for the Hermetic Modular
**Alchemy Lab V2**, on the Alchemy SDK. Written 2026-11-06 before anything
was patched. Claims are marked **✅ tested natively** or **⚠️ unheard /
assumed**; nothing here has run on a module yet.

## 1. Jacks

| Jack | Role | Notes |
|---|---|---|
| J1 | **Clock in** | Codec input, AC-coupled: rising edges only, read sample-accurately in the audio callback (1.0 V up / 0.5 V down). PPQN on the MASTER layer. |
| J2 | **Reset** or **Start/Stop** | Same input type. A held run gate cannot be read (the coupling capacitor drains it in about a second), so "run" is an edge that toggles. |
| J3–J6 | Outputs 1–4 | MCP4728 over I²C, 12-bit, ±5 V, written by the 1 kHz poll when the code changes. ⚠️ timing jitter up to ~1 ms. |
| J7–J8 | Outputs 5–6 | STM32 DAC, 12-bit, ±5 V, written every 0.5 ms from the audio callback. |
| J9–J10 | Outputs 7–8 | Codec, DC-coupled, ±5 V, every 0.5 ms (one 24-frame block). |

Every output is 0–5 V at the default OFFSET of 0; OFFSET moves it anywhere
in ±5 V (−2.5 V gives a ±2.5 V LFO).

## 2. Panel

No screen, so everything is pots, rings and button colours. Three layers;
the pots must **catch** the stored value before they change it (a dim pip
shows where a pot physically sits until it does), because six pots serve
eight outputs, three layers and every firmware on the card.

### OUTPUT layer (base) — the selected output

| Pot | Parameter | Range | Ring |
|---|---|---|---|
| P1 | **RATIO** | /64 /32 /24 /16 /12 /8 /7 /6 /5 /4 /3 /2 /1.5 **x1** x1.5 x2 x3 x4 x5 x6 x7 x8 x12 x16 x24 x32 | dot; x1 marked white |
| P2 | **WAVE** | Gate · Trig · Saw · Ramp · Tri · Sine · Env · Random · Smooth | 9-zone dot |
| P3 | **WIDTH** | gate length (to 100% = tie), trig 5–50 ms, triangle skew, envelope decay | level |
| P4 | **LEVEL** | 0–5 V | level |
| P5 | **OFFSET** | −5…+5 V | bipolar from 12 o'clock |
| P6 | **PHASE** | 0–1 cycle | level |

### PATTERN layer (hold B3)

| Pot | Parameter | Range |
|---|---|---|
| P1 | **STEPS** | off, 1–32 (euclidean length; turning it up from off starts half full) |
| P2 | **FILLS** | 0…steps |
| P3 | **ROTATE** | 0…steps−1 |
| P4 | **CHANCE** | 0–100 % per hit |
| P5 | **SWING** | 50 % (straight) – 75 %; 66.7 % is triplet swing |
| P6 | **SLEW** | 0 – about 1 s glide |

### MASTER layer (hold B2)

| Pot | Parameter |
|---|---|
| P1 | **BPM** 10–300 (ignored while J1 is driving); the ring's last LED flashes on the beat |
| P2 | **Clock PPQN** for J1: 1 · 2 · 4 · 8 · 12 · **24** · 48 |
| P3 | **J2 mode**: Reset · Start/Stop |

### Buttons

| Button | Tap | Hold |
|---|---|---|
| B1 | next output (1→8) | turn P1 to jump to any output (the ring shows all eight in their colours) |
| B2 | tap tempo (2–4 taps averaged; ignored on external clock) | MASTER layer |
| B3 | stop / start (start is from the top) | PATTERN layer |
| B2+B3 | — | 2 s: SDK Settings (brightness, presets; page 2 = SD firmware picker) |

LEDs: B1 = the selected output's colour, lit by its signal; B2 = the beat
(amber internal, cyan locked to J1); B3 = green running, red stopped.
Output colours 1–8: red, orange, yellow, green, cyan, blue, purple, pink.

## 3. Engine (`src/pulse_engine.*`, no SDK)

One double counts quarter-note beats since reset. Each output's cycle count
is `beats × num/den + phase`, swing-warped per pair of cycles; the integer
part drives the euclidean step, the chance roll and the random levels, the
fraction is the waveform phase. So:

- every output and pattern lines up again on reset ✅;
- chance and random sequences replay identically after each reset (seeded
  per output) ✅ — a free "loop": reset every N bars to repeat;
- following J1, each pulse snaps the position to `pulses / PPQN` and the
  outputs never run past the next expected pulse, so multiplications stay
  phase-locked and a late clock holds rather than drifts ✅. Silence for
  3 periods (0.25–4 s) falls back to the internal BPM ✅.

`make test` — 82 checks: ratio counts and grid alignment, duty, levels and
offsets, euclid spacing and rotation, swing timing, phase, chance (0, ~50 %,
repeatability), tap tempo, external clock at 100 BPM / 24 PPQN with x4 and
/4, a tempo change under x8, reset and start/stop on J2, random + slew,
preset round-trip, gates low while stopped.

## 4. Persistence

The whole state (master + 8 × 24 bytes) rides in the SDK preset blob as
`PulseState` ('PLS' schema). Working state autosaves to **slot 11** five
seconds after the last change with hands off the buttons. The family's
other firmwares use 12–15, so switching through the SD picker keeps each
one's state.

First boot: 1 quarters · 2 eighths · 3 sixteenth trigs · 4 bar gates ·
5 two-bar sine ±2.5 V · 6 four-bar triangle ±2.5 V · 7 tresillo (3 in 8)
eighth trigs · 8 random level each beat.

## 5. Not in v0.1

Ratchets, quantizer, CV inputs and assignment, cross-output logic,
triggered (one-shot) mode, humanize / other micro-timing, beat-looped
random, bank loads from the clock inputs, MIDI. The jack budget is the main
constraint: all of J3–J10 are outputs, so CV in means giving some up.
