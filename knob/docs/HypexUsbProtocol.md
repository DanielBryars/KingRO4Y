# Hypex FusionAmp USB HID Protocol

Single-source reference for the Hypex Fusion USB HID control protocol as
it is understood today. Combines:

- The reverse-engineered protocol summary in `../../hypex_fusion_notes.md`
  (originally distilled from a DIYAudio thread, the
  [Turbopsych/UsbAmpControl](https://github.com/Turbopsych/UsbAmpControl)
  GitHub project tested against an FA253, and the vendor PDF).
- The vendor PDF `speakers/vendor/hypex/Hypex USB Hid documentation.pdf`.
- Empirical experiments against an actual FA503 — see
  `knob/docs/experiments.md` for raw data and dated session logs.
- HFD binary inspection: symbol-name strings extracted from
  `HFD.exe` (Delphi/FireMonkey, 18 MB) reveal everything the firmware
  *can* expose, even where the report ID isn't yet known.

Each fact is tagged with one of:

- **VERIFIED** — confirmed by experiment on real hardware
- **DOC** — stated in the vendor PDF, not yet independently checked
- **HFD** — derived from HFD.exe symbol names; the feature exists in
  firmware, exact wire format unknown
- **GUESS** — hypothesis based on partial data, not confirmed
- **UNKNOWN** — listed for completeness; we don't know
- **DANGER** — confirmed or suspected harmful

---

## 1. Hardware

The FA503 is a three-channel Hypex Fusion plate amp (2× 500 W + 1× 100 W).
Internally it identifies as a **DSP3** module (the same DSP family that
also powers the DSP3-224 Stereo DSP). The protocol is shared across the
Fusion family.

| Field | Value | Tag |
|-------|-------|-----|
| USB VID | `0x345e` ("Hypex Electronics BV") | VERIFIED |
| USB PID | `0x03e8` | VERIFIED |
| Product string | `DSP3-213` | VERIFIED |
| Serial number string | `0303A:H05U…-…M…-…` (per unit) | VERIFIED |
| HID usage page | `0xff00` (vendor-defined) | VERIFIED |
| HID usage | `0x0001` | VERIFIED |
| Audio interface (USB Audio Class) | likely also exposed | UNKNOWN |
| Channels | 3 (FA503) / 4 (DSP3-224) | DOC |
| Internal MCU | PIC32 (firmware v5.x) or PIC18 (legacy v1.x) | DOC |

**Bootloader fingerprint**: when the amp is in bootloader / recovery
mode (e.g. after the 0x09 hang), it still enumerates with the same
VID/PID and product string, **but the serial-number string is `None`**
instead of the unit ID. This is the cleanest diagnostic for "main
firmware not loaded". (VERIFIED)

---

## 2. Transport layer

| Property | Value | Tag |
|----------|-------|-----|
| Class | HID (`0x03`) | DOC |
| Packet length | **67 bytes on the wire** for status responses (PDF says 64; trailing 3 bytes are constant `… 80 da 02` and were missed in the original write-up). 64-byte writes from the host work fine. | VERIFIED |
| OUT endpoint | `0x01` (interrupt) | DOC |
| IN endpoint | `0x81` (interrupt) | DOC |
| Report ID | **None** — vendor HID exposes a single unnumbered report | VERIFIED |
| Pattern | Strict command → response. Every OUT must be matched by an IN read. | DOC + VERIFIED |
| Feature reports | Not used by the control protocol; both `get_feature_report` and `send_feature_report` fail. HFD uses them for firmware/project upload (separate path). | VERIFIED |

### hidapi-specific quirk (Python on Windows)

`hidapi.write()` requires a leading `0x00` sentinel byte to indicate
"no Report ID". hidapi strips it before transmit. Without the leading
`0x00`, the first payload byte is consumed as a phantom Report ID,
the on-wire packet is shifted, and the amp returns 64 zero bytes —
which looks superficially like "no response" but is actually the amp
silently rejecting an unrecognised report. (VERIFIED — debugged on
2026-05-02.)

`hidapi.read(64, timeout_ms=…)` returns the 64-byte payload directly
with no Report ID prefix to strip.

### Buffer-drain hazard

If multiple OUT commands are sent without reading the matching IN
responses, IN packets queue up and subsequent reads return *stale*
responses, not the response to the latest command. A `drain` helper
that loops `read()` with non-blocking mode until empty before each
fresh request is good hygiene during exploration. (VERIFIED — caused
the misleading "Restored: preset=1" message on 2026-05-02 that turned
out to be stale buffer; actual state was preset 3.)

---

## 3. Opcode map

The first byte of an OUT packet is treated as an opcode / "request type"
by the amp. The second byte is a sub-code or parameter. Bytes 3+ depend
on the opcode.

| First byte | Class | Status | Notes |
|------------|-------|--------|-------|
| `0x03` | Read (string) | Safe | `0x03 0x08` returns the project filename. Other sub-codes return an empty `03 00 …` template. |
| `0x04` | Read (constant) | Safe | Always returns `04 01 00 00 …`. Looks like a ping / ack. |
| `0x05` | **Write (Set State)** | Destructive — atomic | Commits all four state fields at once. See section 4. |
| `0x06 0x01` | Read (capability block) | Safe | Per-channel hardware constants. Static. |
| `0x06 0x02` | Read (status) | Safe | Primary state read. Returns response type `0x05`. See section 5. |
| `0x06 0x03` | Read (calibration block) | Safe | Per-channel calibration constants. Static. Response type `0x66`. |
| `0x06 NN` (other) | Read | Safe | Returns trivial / empty. Not currently used. |
| `0x07` | Untested | **DO NOT SEND** | Adjacent to known-dangerous `0x09`. |
| `0x08 NN` | Read (live counters) | Safe | Live counters / boot count. See section 7. |
| `0x09` | Untested | **DANGER — DO NOT SEND, EVER** | `0x09 0x00` hung the amp's USB stack and required a power-cycle + firmware re-flash + project re-upload to recover. See `feedback_hypex_dangerous_opcodes.md`. |
| `0x0a`–`0x64` | Untested | **DO NOT SEND** | Same risk class as `0x09` until proven otherwise. |
| `0x65 0x20 NN MM` | Read (parameterised info) | Safe — observed in HFD | Multi-purpose info read. NN selects the field. See section 12. |
| `0x66` | Read/write (calibration?) | Safe — observed in HFD | HFD writes a packet starting `66 00 00 01 …` and reads `66 00 …` back; payload looks like the same bytes returned by `0x06 0x03`. Section 13. |
| `0x67` | Read | Safe — observed in HFD | Returns ~36 bytes of `0xff`, then zeros. Likely an EEPROM dump or a "blank" capabilities probe. Section 13. |
| `0x68`–`0xff` | Untested | **DO NOT SEND** | Until proven otherwise. |

### Operational rules (derived from incidents)

- **No autonomous probing of unknown opcodes.** New first-byte
  opcodes are user-present-only.
- **Probe scripts must declare their allowed opcodes explicitly.**
  `experiment_probe_reports.py` is currently restricted to
  `{0x03, 0x04, 0x06, 0x08}`.
- **Set State writes always round-trip current state.** Use
  `hypex_probe.py`'s `set_state()` helper.
- **After every write, verify with a fresh `06 02` read.** Don't trust
  the immediate response packet alone.
- **Volume cap during exploration:** −30 dB unless the user has
  explicitly authorised higher.

---

## 4. Set State (`0x05`) — destructive write

Sent on the OUT endpoint as a single 64-byte packet. **All four state
fields are committed atomically.** A field "left at zero" is *applied
as zero*, not "leave alone".

```
byte 0: 0x05            (opcode)
byte 1: input_source    (uint8, see below; 0x00 = "no change")
byte 2: preset          (uint8, 1..3)
byte 3-4: volume        (int16 LE, dB × 100)
byte 5: 0x00            (reserved)
byte 6: mute_flag       (bit 7: 0x80 = muted, 0x00 = not)
byte 7-63: zero-padded
```

(Source: vendor PDF; verified on FA503.)

### Input source enum (byte 1)

| Value | Input | Tag |
|-------|-------|-----|
| `0x00` | SCAN / no-change — keeps the currently active source | DOC |
| `0x01` | XLR | DOC |
| `0x02` | RCA | DOC |
| `0x04` | SPDIF (coaxial) | DOC |
| `0x05` | AES | DOC |
| `0x06` | OPT (Toslink) | VERIFIED |

Always send `0x00` when you only want to change the volume / preset /
mute and not the input — otherwise the amp will switch input and
likely lose audio. (DOC + VERIFIED)

### Volume encoding

```
register_value = round(dB × 100)
# stored as int16 LE in bytes 3-4
```

| dB | Value | Bytes 3-4 |
|------|------|-----------|
| 0.0 | 0 | `00 00` |
| −3.0 | −300 | `D4 FE` |
| −10.0 | −1000 | `18 FC` |
| −30.0 | −3000 | `48 F4` |
| −40.0 | −4000 | `60 F0` |
| −50.0 | −5000 | `78 EC` |
| −60.0 | −6000 | `90 E8` |
| −99.0 (min) | −9900 | `54 D9` |

(VERIFIED — the −30 / −40 / −50 / −60 cases all round-tripped through
the live amp during 2026-05-02 sessions.)

### Mute (byte 6)

Bit 7 is the only bit observed to do anything: `0x80` = muted,
`0x00` = not muted. Bits 0–6 stayed zero in every response captured
across mute / volume / preset / input variation. (VERIFIED.)

### Rate limit

Two consecutive Set State writes ~50 ms apart can result in the
**second one being silently dropped** — no error returned, no protocol
indication, the amp just doesn't apply it. (VERIFIED — 2026-05-02 saw
a v=−40 dB write fail to take after a v=−50 dB write.)

The HFD pcap on 2026-05-03 narrows the floor: HFD's own polling loop
ran at 8 Hz (mean gap 125 ms) with **a minimum observed gap of 53 ms**
across 248 packets, and HFD never had a write silently dropped. So
the safe minimum gap for back-to-back state-changing writes is
somewhere between **50 ms and 80 ms**. Recommended firmware throttle:
**100 ms minimum** between consecutive Set State writes; the slider's
30 ms debounce should be raised. (VERIFIED.)

### The "polling" sub-command (`0x05 0x01` vs `0x05 0x00`)

Opcode `0x05` is *overloaded*. Byte 1 distinguishes two modes:

- **`0x05 0x00 …`** — Set State (destructive, atomic). Byte 1 = 0
  means "input source: no change"; the rest of the packet sets state.
- **`0x05 0x01 …`** — **VU-meter polling request**. Byte 1 = 0x01 is
  *not* "set input to XLR" in this position; it's a sub-command that
  asks the amp to return state plus current peak meter values. The
  rest of the packet must mirror the amp's *current* state (preset,
  volume, project signature) so the amp can verify the host is in
  sync. The IN response is a 67-byte status frame with peak meter
  values populated in bytes 47–49 (see section 5).

This was discovered from the HFD pcap on 2026-05-03; the vendor PDF
does not document it. (VERIFIED.)

---

## 5. Get Status (`0x06 0x02`) — primary state read

Request: `0x00 0x06 0x02 0x00 0x00 …` (with the leading hidapi `0x00`
sentinel; on the wire that's `0x06 0x02 0x00 0x00 …`).

Response: 64 bytes, response type `0x05` in byte 0.

### Decoded bytes (verified)

| Byte(s) | Field | Notes |
|---------|-------|-------|
| 0 | Response type | Always `0x05` for a status response |
| 1 | Packet ID | Echoes the request type — `0x06` for get-status, `0x00` for the synchronous response after a Set State |
| 2 | Current preset | 1, 2, or 3 |
| 3–4 | Current volume | int16 LE, dB × 100 |
| 5 | Reserved | Always 0 in our captures |
| 6 | Status flags | Bit 7 = mute. Other bits unused. |
| 50 | **Active input source** | Same enum as in Set State byte 1. Confirmed `0x06` after switching to OPT. *Not documented in the vendor PDF — discovered empirically.* |

### Bytes that change but are not yet decoded (GUESS / UNVERIFIED)

| Byte(s) | Behaviour | Best guess |
|---------|-----------|------------|
| 8–11 | Constant within one project, e.g. `0f 09 00 02` for the user's KingRO4Y project; was `0f 19 00 02` before the post-incident project re-upload | **GUESS: project / DSP signature. HFD echoes this back in every polling packet so the amp can detect project mismatches.** |
| 21–22 | Echo the volume target, but lag the actual fader during ramps | "Volume mirror" — possibly the per-output trim post-DSP |
| 24–26 | Constant `ff ff ff` in IN responses, `ff ff 00` (last byte 0) in HFD's polling OUT | **GUESS: one of these bits is a "last write valid" flag the amp toggles** |
| 35–36 | Constant `f6 01` (= 502 LE) across all captures we have | **GUESS: probably a project / firmware version code** |
| 45–46 | Constant `e0 01` (= 480 LE) across the 248 polling responses | **GUESS: peak-meter scale endpoint or sample-count constant** |
| **47–48** | **Vary across every polling response — `0x05/0x80` to `0x55/0x55` typical, range as LE16 ≈ 1280..21800.** | **VU METER. Linear peak scale 0..32767 ≈ 0..0 dBFS. A reading of ~9000 = -11 dB FS, ~3000 = -21 dB FS.** See section 5a. |
| 49 | Range 0..85 — varies with audio level but more slowly than 47–48 | **GUESS: peak-hold counter or a second VU value (per-channel?).** |
| 51 | Mirrors current preset | Redundant copy |
| 52–53 | Lag the commanded volume by ~1–2 captures | "Actual fader position" during soft ramp (vs commanded target at bytes 3–4) |
| 56–57 | `3e 3a` constant in HFD's traces | UNKNOWN |
| 60 | Three values across captures: `0x00`, `0x40`, `0xc0` — bit 6 = "audio active", bit 7 = ? | **GUESS: bit 6 = audio output enabled, bit 7 = signal lock (e.g. SPDIF lock). When both clear, the amp is silent.** |
| Tail (61, 62, 63, 64) | Constant `00 80 da 02` across all captures | Probably packet framing / version marker — ignore |

### 5a. VU meter encoding (decoded from HFD pcap)

Across 248 polling responses captured while music was playing:

- **Byte 47 (LE low) and byte 48 (LE high)** form a 16-bit unsigned
  value in the range **1280..21763** (varying with audio amplitude).
- Interpreting as linear peak in `[0, 32767]` mapped to `[0, 0 dB FS]`:

```
dB_FS = 20 * log10(value / 32768.0)
```

  Yields plausible audio peaks: 1280 → −28 dB FS, 9000 → −11 dB FS,
  21763 → −3.5 dB FS. (GUESS, but the range and dynamics fit.)

- **Byte 46** also varies dynamically (114 unique values). Could be a
  second meter (e.g. input vs output), an envelope follower with
  different ballistics, or per-channel data. Needs a per-channel
  controlled experiment to confirm.

- **Byte 49** (range 0..85) tracks audio level more slowly. Likely
  peak-hold or a per-channel sample.

The full per-channel meter set (input L/R + 3 output channels, as
HFD's `LblDspPeakOutCh1..Ch4` symbols imply) probably lives in the
`0x65 0x20 (a0/c0/e0) NN` reads — see section 12. Bytes 47–49 of the
status response appear to be a **single summed level**, perfect for a
single-bar VU.



The full decode of these bytes is on the experiment shopping-list in
`experiments.md`, but no longer urgent given the knob's core requirements
already work.

---

## 6. Filter / project name (`0x03 0x08`) — read

Request: `0x03 0x08 0x00 0x00 …`

Response: 64 bytes. Byte 0 = `0x03`. Byte 1 = `0x00` (status / length /
unknown). Bytes 2 onwards: NUL-terminated ASCII string.

Important caveat: the string returned is the **HFD project filename**
(e.g. `Config.xml`), not a friendly preset name. If you want
human-friendly preset labels on the knob, either rename the HFD project
file or find a different per-preset name request (UNKNOWN — possibly
in `0x03 NN` for an `NN` we haven't tried). (VERIFIED.)

When no project is loaded (e.g. after a bootloader reset), the response
is the empty template `03 00 00 00 …`. This is a clean diagnostic for
"DSP project unloaded". (VERIFIED — that's how we identified the
post-incident state on 2026-05-03.)

Other `0x03 NN` sub-codes return the same empty template; their
meanings are UNKNOWN.

---

## 7. Live counters (`0x08 NN`) — read

Request: `0x08 NN 0x00 0x00 …` for `NN` in `0x00..0x0f`. The response
shape is the same regardless of `NN`; the `NN` value either selects a
sub-report or is ignored.

Response example (preset 3, FA503 idling):

```
08 01 f1 1a 00 00 a4 19 00 00 08 00 00 00 00 02 00 00 00 00 …
```

| Byte(s) | Field | Tag |
|---------|-------|-----|
| 0 | Response type — always `0x08` | VERIFIED |
| 1 | Sometimes alternates `00`/`01`; meaning UNKNOWN | UNVERIFIED |
| 2–3 | **Counter A**, int16 LE, increments over time | VERIFIED |
| 4–5 | Always `00 00` in our captures | UNKNOWN |
| 6–7 | **Counter B**, int16 LE, also increments — incremented by 1 at the same probe sample as Counter A in a 16-call sweep, suggesting they're tied | VERIFIED |
| 8–9 | Always `00 00` | UNKNOWN |
| 10 | **Suspected boot counter** — was `0x08`, became `0x09` after the recovery cycle | GUESS |
| 11 | Always `00` | UNKNOWN |
| 12–15 | Constant `00 00 00 02` | UNKNOWN |
| 16+ | Mostly zero | UNKNOWN |

This is **the only confirmed live-data report we've found**. It does
not look like a meter — meters fluctuate at audio rate and have a
range; these are monotonic counters. They are most plausibly an
**audio frame counter or sample-rate-derived clock**, which would
explain why they increment together: both are tied to the same audio
clock. If audio frames stop arriving (no SPDIF lock, mute-equivalent
state), the counters might freeze — that would be a clean
**signal-presence** indicator for the knob.

This hypothesis is **GUESS-level**. Confirming it would take a
~10 second focused experiment: capture 100 responses with audio on,
then 100 with the SPDIF source unplugged, compare counter rates.

---

## 8. Capability block (`0x06 0x01`) — read, static

Request: `0x06 0x01 0x00 0x00 …`

Response example:

```
06 16 05 01 e2 03 0f 0f 0f 20 20 20 03 05 81 05
00 07 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 94 11 dc 05 dc 05 dc 05 00 00 00 00 00 00
```

| Bytes | Likely meaning | Tag |
|-------|----------------|-----|
| 0 | Response type `0x06` | VERIFIED |
| 1 | `0x16` (22) — possibly length | GUESS |
| 4–5 | `e2 03` LE = 994 — UNKNOWN | UNKNOWN |
| 6–8 | `0f 0f 0f` — per-channel triplet, value 15 | GUESS — per-channel constant |
| 9–11 | `20 20 20` — per-channel triplet, value 32 | GUESS |
| 12 | `03` — possibly channel count | GUESS |
| 50–51 | `94 11` LE = 4500 — UNKNOWN | UNKNOWN |
| 52–53, 54–55, 56–57 | `dc 05 dc 05 dc 05` — per-channel triplet, value 1500 each | GUESS — per-channel limit (e.g. max output power, max voltage trim, etc.) |

**Static** across 25 captures with varied mute/volume/preset on
2026-05-02. So this is configuration / capability data, not live
state. (VERIFIED.)

If the values differ between the two FA503s in the speaker pair, the
varying ones are unit-specific (factory calibration). If they're the
same, they're model constants.

---

## 9. Calibration block (`0x06 0x03`) — read, static

Request: `0x06 0x03 0x00 0x00 …`

Response example:

```
66 00 00 00 00 00 00 00 00 00 05 09 16 01 01 41
00 10 00 00 1f 85 ab 02 1f 85 ab 02 29 5c 0f 05
57 ec ff 00 57 ec ff 00 57 ec ff 00 b9 fc ff 00
b9 fc ff 00 b9 fc ff 00 28 28 28 14 14 14 00 00
```

| Bytes | Likely meaning | Tag |
|-------|----------------|-----|
| 0 | Response type `0x66` (different from `0x05`/`0x06`) | VERIFIED |
| 10–11 | `05 09` — version-like | GUESS |
| 12–14 | `16 01 01` — UNKNOWN | UNKNOWN |
| 15 | `0x41` — could be ASCII `'A'` (revision letter?) | GUESS |
| 20–23 and 24–27 | Two copies of `1f 85 ab 02` (LE = 0x02ab851f = 44 729 119) | GUESS — uptime in seconds, date code, or a calibration value |
| 28–31 | `29 5c 0f 05` (LE = 0x050f5c29 = 85 000 233) | UNKNOWN |
| 32–43 | Three copies of `57 ec ff 00` — per-channel | GUESS — per-channel 24- or 32-bit calibration value |
| 44–55 | Three copies of `b9 fc ff 00` — per-channel | GUESS |
| 56–58 | `28 28 28` (= 40 each) — per-channel | GUESS — *possibly* per-channel temperature calibration constant, or a per-channel limit set in degrees C |
| 59–61 | `14 14 14` (= 20 each) — per-channel | GUESS |

**Static** across all 25 captures on 2026-05-02. This contradicts our
initial hypothesis that `0x06 0x03` was a live sensor block — at
seconds-to-minutes timescales it does not change. It might still
change on hours-scale (temperature drift) but we haven't tested for
that. (VERIFIED for short timescales.)

---

## 10. Telemetry HFD has access to but we haven't yet decoded

From symbol-name strings extracted from `HFD.exe` (Delphi RTTI). The
firmware *does* expose all of these — HFD reads them — but we don't
yet know which report ID(s) carry them. (Tag throughout: HFD.)

### Status bits (StatusBits*)

A bitfield with at least these named bits:

```
AmpEnabled                 deviceJustBooted
passwordMatchedHfd         prevButtonPressed
signalDetected             updateFiltersBusy
switchDSPOff               testsEnabled
signalPending              signalLostPending
StandbyPending             SystemUnhealty   (sic, "Unhealthy" misspelt)
SignalDetectedChanged
```

`signalDetected` is the headline one — that's the answer to "is audio
actually playing right now". `StandbyPending` would let the knob warn
the user. `deviceJustBooted` lets the knob re-establish state cleanly.

### Health bits (HealthBits*)

A second bitfield, errors and warnings:

```
TempSensorFailError        TempTooHighError
VauxOutOfRangeError        DCErrorError       (sic, doubled "Error")
SRCI2CError                PowerFailureError
I2CbusError                TemperatureWarning
AnalogeClipWarning         DigitalClipWarning   (sic, "Analoge")
```

`AnalogeClipWarning` and `DigitalClipWarning` are the clip indicators
for a VU meter.

### Per-input signal detection

```
rcaSignalDetected          xlrSignalDetected
dspSPDIFSignalDetected     srcSignalDetected
```

So per-input signal presence is queryable separately from the global
`signalDetected` status bit.

### Per-channel temperature

`Temperature 1`, `Temperature 2` (FA503 has two sensors; DSP3-224 up
to four). Also `TemperatureSensor1` / `TemperatureSensor2`.

### Per-channel input + output peak meters

```
LblDspPeakLEFTIn           LblDspPeakRIGHTIn
LblDspPeakOutCh1           LblDspPeakOutCh2
LblDspPeakOutCh3           LblDspPeakOutCh4
```

So the amp returns **both input peak (L/R after the SRC) and output
peak (per amp channel, post-DSP)**. There's also a `FusionAmpDSPPeakSetpoint`
which is presumably the threshold above which the digital-clip warning
fires.

### Other readable properties

```
FusionAmpActualVolume         FusionAmpHealth
FusionAmpEeprom               FusionAmpForceInput
FusionAmpEnableMFB            FusionAmpDetectedHardware
FusionAmpThirdChannelEnabled  FusionAmpFanConnected
FusionAmpStereoDSP            FusionAmpMixerPreset
FusionAmpTemperatureSetpoint  FusionAmpLimiterSettings
FusionAmpPasswordProtected    FusionAmpPasswordMatched
FusionAmpPresetSettings
```

`FusionAmpEeprom` is interesting — that's likely a window into the
amp's persistent config, including per-preset filter coefficients and
EQ. `FusionAmpLimiterSettings` is the soft-clip limiter (HFD release
notes mention it). `FusionAmpFanConnected` would be a useful "fan
running" indicator.

### Settable properties HFD writes via Set State or other commands

```
SetFusionAmpPreset            SetFusionAmp32Preset
SetFusionAmpChannelBiquad     SetFusionAmpChannelDelay
```

`SetFusionAmpChannelBiquad` and `SetFusionAmpChannelDelay` are the
filter-coefficient-write commands. These are in opcode space we
haven't probed (probably above `0x05`); they're how HFD uploads a
project. Outside the knob's needs.

### "acd*" enum (probably "amp control data")

```
acdInputMonitor    acdInputGain    acdInputMeter
acdOutputGain      acdOutputMeter  acdTimeInfo
acdTimeCode        acdTransport
```

The integer values of these enum members would correspond to report
sub-codes. The values themselves aren't stored as strings in the
binary, so finding them requires either disassembly or a USB packet
capture.

---

## 12. Parameterised info read (`0x65 0x20 NN MM`)

Newly observed in HFD pcap. The first two bytes `0x65 0x20` select an
"info" report; bytes 2 and 3 select sub-fields. The response shape and
length vary per sub-field. Confirmed safe (HFD uses these freely).
(VERIFIED.)

| Request bytes | Response (excerpt) | Decoded |
|---------------|--------------------|---------|
| `65 20 00 01 …` | `65 20 48 30 35 55 30 30 33 32 36 38 2d 30 35 30 30 4d 36 35 37 39 2d 30 30 35 32 00 …` | **Serial number string** as ASCII at byte 2. Matches the unit serial we already see in `hid.enumerate()`. |
| `65 20 20 01 …` | `65 20 46 41 35 30 33 00 …` | **Model name string** at byte 2: `"FA503"`. |
| `65 20 40 01 …` | `65 20 00 00 …` | Empty in our capture. Purpose UNKNOWN. |
| `65 20 a0 5N 02 01 …` | `65 20 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 80 88 13 00 c3 02 00 00 80 10 27 00 …` | UNKNOWN — small flags, then a constant `13 00`, then values. |
| `65 20 c0 5N 02 01 …` | `65 20 c3 02 00 00 80 20 4e 00  c3 02 00 00 80 50 c3 00  c3 02 00 00 80 a0 86 01  c3 02 00 00 80 40 0d 03 …` | **Per-channel triplet of 8-byte records** (4 records, each `c3 02 00 00 80 XX XX XX`). Triple matches FA503's 3 channels plus possibly a sum. The `c3 02 00 00` prefix is constant; the trailing 4 bytes vary per record. **GUESS: per-channel limiter / clip / setpoint state.** |
| `65 20 e0 5N 02 01 …` | `65 20 c3 02 00 00 80 20 a1 07  …  ff ff 5d 3e …` | Same shape as `c0` but with larger values; **GUESS: a different field in the same per-channel record.** |
| `65 20 (a0|c0|e0) (50|60|70|80)` | (each forms a 3-tuple) | HFD reads these in synchronised groups every ~2 s — looks like a periodic per-channel telemetry sweep complementary to the fast `0x05 0x01` polling. |

Note the second nibble of byte 3 (the `5`/`6`/`7`/`8` in `50/60/70/80`)
appears to advance through some kind of channel or page index.

## 13. Calibration write (`0x66`) and EEPROM-ish read (`0x67`)

Both observed once each in HFD's startup handshake.

`0x66`: HFD wrote `66 00 00 01 00 00 00 00 00 00 05 09 00 01 01 00 00
00 00 00 2c 2d f0 08 ff ff 00 …` — the body matches the data the amp
returns from `0x06 0x03` (the calibration block). Likely a
"calibration confirm / write-through" in HFD's bootstrap. Byte 3 = 1
means "write" vs the read-side `0x66 0x00 0x00 0x00`. (GUESS.)

`0x67`: HFD wrote `67 00 00 00 …` (zero body) and got back
`67 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 00 00 …` — 36 bytes of
`0xff`, then zeros. Probably an **EEPROM read where the addresses are
unprogrammed** (`0xff` = blank flash / EEPROM) or a probe for OEM /
license bytes. (GUESS.)

Neither is needed for the knob — listed for completeness.

## 14. Known dangerous opcodes

### `0x09 NN` — DO NOT SEND

On 2026-05-02 a probe of `0x09 0x00 0x00 0x00 …` caused the FA503 to:

- Throw `OSError: read error` from hidapi on the read.
- **Vanish from `hid.enumerate()` for >2 minutes** — the USB stack hung
  outright.
- Lose its DSP project from RAM (the `0x03 0x08` filter-name read
  returned empty after recovery).
- Required: mains power-cycle (≥30 s off), USB cable replug, HFD
  detection, **firmware re-flash** to the latest version, and project
  re-upload from `Config.xml`.

The vendor recovery procedure is documented in
`feedback_hypex_recovery_procedure.md` and the experiment log.

**Best guess** for what `0x09` is: a bootloader / firmware-update entry
point that left the main MCU in an inconsistent state when its
preconditions weren't met. (GUESS.)

### `0x07`, `0x0a`+, `0x10`+ — assume same risk class

Untested. Until proven otherwise, treat as potentially hangs-the-amp.

---

## 15. Recovery procedure (if the amp goes silent / drops off USB)

Canonical steps, proven 2026-05-03:

1. **Mains off for ≥30 s.** Capacitor discharge matters; quick
   toggles fail.
2. Power on; wait 20–30 s for USB stack.
3. If amp shows on USB but HFD can't see it: **unplug + replug USB
   cable** to force Windows HID re-enumeration.
4. Open HFD. **Accept any firmware update offered.**
5. Upload the project from `speakers/dsp/presets/KingRO4Y_MKIII_Presets/Config.xml`.
6. Volume comes back at factory default (high). Drop it before unmuting.
7. Use `knob/software/tools/hypex_keepalive.py` if needed to keep the
   amp out of standby while HFD is in use.

Diagnostic fingerprints that distinguish recovery states:

| Signal | What it means |
|--------|---------------|
| Amp not in `hid.enumerate()` at all | USB stack hung; needs power cycle |
| Enumerated but `serial_number_string` is `None` | Bootloader mode — main firmware not running |
| `0x03 0x08` returns empty | DSP project unloaded |
| `0x06 0x02` returns volume `0` and mute `0x80` | Factory defaults (post bootloader reset) |
| `0x08 0x00` byte 10 incremented vs last known | Amp did a clean reboot at some point |

If the amp doesn't re-enumerate at all after ≥2 minutes on a clean
power cycle, downgrade HFD to **4.97** (community workaround for stuck
bootloader). If that still fails, email **support@hypex.nl** — they
can re-flash the PIC manually.

---

## 16. References

- Vendor PDF: `speakers/vendor/hypex/Hypex USB Hid documentation.pdf`
- Local notes: `hypex_fusion_notes.md` (repo root) — protocol summary
  predating the experiments.
- Experiment log: `knob/docs/experiments.md` — raw captures, dated
  sessions, byte-diff tables.
- HFD bundled at: `C:\Program Files (x86)\Hypex Software\Hypex filter design 5.2.4.24\HFD.exe`
- HFD strings helper: `knob/software/tools/_hfd_strings.py`
- Reference implementation (ESP32-S3, MIT):
  [Turbopsych/UsbAmpControl](https://github.com/Turbopsych/UsbAmpControl)
  — tested on FA253 firmware v5.7. Same Fusion family, same protocol.
- diyAudio thread for UsbAmpControl:
  <https://www.diyaudio.com/community/threads/esp32-s3-based-fusion-amp-controller-with-ab-and-abx-testing-support.430952/>
- diyAudio thread on the OLED display (different protocol —
  undocumented "UART-like"):
  <https://www.diyaudio.com/community/threads/hypex-fusion-remote-kit-and-led-display-how-do-they-look.421814/>
- HFD release notes confirming a USB VU meter feature exists:
  `Documentation/Releasenotes hfd v5.2.4.txt`, line 3 —
  *"USB connection sometimes 'hanged' using VU meter, this is fixed"*.

---

## 17. Open questions, in priority order

1. **Per-channel VU meters via `0x65 0x20`.** We have a single-channel
   VU at status bytes 47–48; per-channel input + output peaks live in
   the `0x65 0x20 (c0/e0) NN` triplets per section 12. Need to decode
   the 8-byte record format inside.
2. **Status bits and Health bits report ID.** Still unknown which
   `0x0X 0xNN` carries them. A second pcap with HFD's "Status" panel
   open (showing protection/temp warnings) would reveal it.
3. **Linear-peak vs dB-FS scaling for bytes 47–48.** Our hypothesis is
   linear `[0, 32767] → [-∞, 0] dB FS`. Should be verified by playing
   a tone at a known level (e.g. -20 dB FS pink noise) and reading
   back the value.
4. **Per-preset friendly name** — find the `0x03 NN` (or other) that
   returns the user-friendly preset name HFD shows in its UI. The
   `0x03 0x08` we know returns the *project* filename.
5. **Master/slave behaviour** between two FA503s — does the slave
   follow volume / preset / mute over the digital chain, or do we
   need dual-host USB?
6. **Decode of `08 NN` counters** — frame counter or free-running
   clock? A controlled signal-on / signal-off test would distinguish.
