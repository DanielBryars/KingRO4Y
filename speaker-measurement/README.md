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
| `audio_lib.py`| Shared helpers: device lookup by name + host API, and concurrent play/record across two different devices (separate streams in threads). |

Change the device names or `MIC_CH` at the top of `measure.py` / `sanity.py` if the rig changes.

## Result (first speaker, 2026-07-14)

- **Connectivity:** OK. Speaker responds on the LEFT channel; mic on Scarlett input 1.
- **Polarity:** `+` (normal) — impulse response peaks positive.
- **−6 dB band:** ~68 Hz … ~12.9 kHz (in-room, at the mic position used).
- **Midband ripple:** ~±7 dB, 100 Hz–10 kHz — much of the low/low-mid bumpiness (notably a lift
  around 80–250 Hz) is **room modes / boundary reinforcement**, not the driver. Move the mic or
  gate tighter to separate speaker from room.

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
