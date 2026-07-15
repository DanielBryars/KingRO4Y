# Speaker measurement

Quick acoustic test rig for the powered speakers: log-sine-sweep (Farina) measurement of
frequency response, minimum-phase, and polarity, using a calibrated mic on a Focusrite.

## Hardware / signal chain

- **Output:** PC digital (SPDIF) out → `Digital Output (Realtek USB2.0 Audio)`. Speaker under
  test is on the **LEFT** channel.
- **Input:** calibrated mic → **input 1** of `Scarlett 2i2 USB`. **48V phantom power must be ON**
  (the mic is a condenser — with phantom off it reads as digital silence).
- Both run over WASAPI at 48 kHz. Output and input are on independent clocks (see caveats).

## Setup

```
python -m pip install sounddevice numpy scipy matplotlib
```

## Scripts (run from this folder)

| Script | What it does |
|---|---|
| `miccheck.py` | Records 2 s of ambient from the Scarlett across every host API; prints per-channel dBFS. Use this to confirm the mic is live (tap it — level should jump well above −60 dBFS). |
| `sanity.py`   | Plays a 1 kHz tone on LEFT then RIGHT while recording the mic; confirms wiring and channel mapping. |
| `measure.py`  | Plays a 20 Hz–20 kHz log sweep on LEFT, records the mic, deconvolves to an impulse response (`ir.npy`), prints a 1/6-oct frequency-response table, and saves `speaker_response.png`. |
| `analyze.py`  | Re-analyses `ir.npy` only (no audio) → magnitude, minimum-phase, polarity, band edges. Regenerates `speaker_response.png`. |
| `capture_channel.py` | `python capture_channel.py L\|R out.npy` — sweeps one channel, saves its impulse response. The building block for all comparisons. |
| `compare_speakers.py` | Overlays `ir_left.npy` vs `ir_right.npy` **preserving the level difference** (per-curve normalizing would hide a level mismatch). |
| `analyze_gated.py` | Time-gated (quasi-anechoic) response — strips room reflections so crossover integration is visible. Trust it only above ~300 Hz. |
| `analyze_positions.py` | Averages `pos*_L/R.npy` over several mic spots. Features stable across positions = the speaker; features that wander = the room. |
| `compare_presets.py` | Overlays gated responses of presets 1/2/3. |
| `check_damage.py` | `python check_damage.py reference.npy new.npy` — compares response **and harmonic distortion (H2–H5)** against a known-good capture. See below. |
| `audio_lib.py`| Shared helpers: device lookup by name + host API, and concurrent play/record across two different devices (separate streams in threads). |

Change the device names or `MIC_CH` at the top of `measure.py` / `sanity.py` if the rig changes.
Device *indices* shuffle when hardware is re-plugged — everything looks devices up **by name**, so that's handled.

## Reference captures (on-axis, 95 cm, tweeter height)

| File | What |
|---|---|
| `onaxis_R.npy` | RIGHT speaker, healthy baseline (2026-07-14). Best SNR (IR peak 79.4) — the primary reference. |
| `onaxis_R_ref_2026-07-15.npy` | RIGHT speaker after the accidental full-volume event. Verified undamaged: response within ±0.4 dB of baseline 200 Hz–8 kHz, distortion unchanged. |
| `onaxis_L.npy` | LEFT speaker with the faulty tweeter — dead above ~2.5 kHz. Kept as an example of what a failed tweeter looks like. |

## Damage check workflow

1. Put the mic **on the speaker's tweeter axis at 95 cm**, tweeter height, pointed straight at it.
2. Set a **normal listening level** — aim for the capture to land near **−32 dBFS peak**. Level matters: too quiet and the harmonics sink below the noise floor and the distortion test says nothing.
3. `python capture_channel.py R new.npy` then `python check_damage.py onaxis_R.npy new.npy`.

**Reading the distortion output — the noise floor line is the key.** If H2–H5 all sit at roughly the same value as `noise`, you are measuring noise, not distortion; the result is void — raise the level and re-run. Only harmonics clearly *above* the floor mean anything. Real damage **reproduces on every run** — run the capture 2–3× before believing any alarming number. A one-off spike is a dropout or a stray sound, not a driver fault (this bit us on 2026-07-15).

## Result (first speaker, 2026-07-14)

- **Connectivity:** OK. Speaker responds on the LEFT channel; mic on Scarlett input 1.
- **Polarity:** `+` (normal) — impulse response peaks positive.
- **−6 dB band:** ~68 Hz … ~12.9 kHz (in-room, at the mic position used).
- **Midband ripple:** ~±7 dB, 100 Hz–10 kHz — much of the low/low-mid bumpiness (notably a lift
  around 80–250 Hz) is **room modes / boundary reinforcement**, not the driver. Move the mic or
  gate tighter to separate speaker from room.

## Log

- **2026-07-14 — right speaker: dead mid+tweeter.** Only the woofer played; matched the left below
  1 kHz, then 30–60 dB down above. Tweeter was dead → sent for repair.
- **2026-07-14 — repaired tweeter wired in reverse polarity.** Showed as a deep (~15 dB) cancellation
  notch at **1.6 kHz** (the mid→tweeter crossover) that **did not move when the mic moved** — that's
  the tell that separates a driver fault from a room reflection. Correcting the polarity filled it in.
- **2026-07-14 — tweeter then went silent after re-termination.** On-axis at 95 cm it was 25–50 dB down
  above 2.5 kHz, having worked (in reverse polarity) minutes earlier. Intermittent ⇒ internal fault
  (voice-coil lead-out / terminal joint). Returned to SEAS.
- **2026-07-15 — accidental full-volume event, right speaker: no damage.** Response within ±0.4 dB of
  the healthy baseline (200 Hz–8 kHz); distortion unchanged. An initial "H3/H5 raised 20 dB" reading
  was a one-off dropout — it never reproduced across 3 repeat runs. Also: a "silent speaker" scare
  turned out to be the mic simply unplugged (see Gotchas).

## Gotchas that have bitten us

- **Mic reads digital silence (RMS < −95 dBFS)?** It's never the speaker. Causes, in order:
  48V phantom power off (it does **not** survive a power-cycle on the 2i2), mic unplugged, gain down.
  A live mic in a quiet room reads ~−70 dBFS ambient. Always run `miccheck.py` before believing a
  silent sweep.
- **A huge HF difference between two speakers** is usually **mic position**, not the driver — the top
  octave is razor-directional. Compare speakers **on-axis at a matched distance**, never from one
  asymmetric spot.
- **Level changes invalidate distortion comparisons.** Distortion is level-dependent, and a quiet
  capture buries the harmonics in noise. Match the drive level to the reference.

## Caveats

- **In-room, not anechoic.** The transfer function uses a 50 ms window around the direct sound, so
  reflections still contaminate the low end. Treat the broad midband trend as real; treat narrow
  low-frequency peaks/dips as position-dependent.
- **No loopback timing reference.** Output (Realtek) and input (Scarlett) are separate clocks started
  asynchronously, so absolute delay, group delay, and absolute phase are **not** reliable — the raw
  deconvolution delay is dominated by buffering, not acoustics. The phase plot is the *minimum-phase*
  response derived from the measured magnitude (legitimate for the speaker itself). **Polarity is
  reliable** (from the IR peak sign). For true phase/impulse timing, add a loopback: feed the Realtek
  output back into Scarlett input 2 and use it as the time reference.
