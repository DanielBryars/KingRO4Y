# Knob Experiments Log

A running log of work on the knob subproject. Append new sessions at the bottom.

> **STATUS (2026-05-03):** Amp recovered. Project re-uploaded via HFD after
> a firmware update. Normal operation restored. Recovery procedure and
> updated safe-opcode policy documented in the 2026-05-03 entry below.
> **Read the "Safe-Opcode Policy" section before running any new probe.**

## Project goal

Build a hardware control knob for the KingRO4Y speakers. The two FA503 plate
amps will run in master/slave mode (TBC), so the knob talks USB HID to one
amp and the other follows. Hard requirement: the knob must read the current
volume back from the amp so the display stays in sync regardless of how the
amp's state was last changed (HFD, IR remote, etc.).

## Current state — verified working

- The FA503 USB HID protocol described in
  `speakers/vendor/hypex/Hypex USB Hid documentation.pdf` is accurate against
  our actual amp.
- We can read current preset, volume (in dB), mute state, and active input
  source from the amp.
- We can set volume, preset, mute, and input atomically.
- Python probe scripts in `knob/software/tools/` exercise all of the above
  on Windows over hidapi.

## Hardware identifiers (FA503 in this build)

- **USB VID:** `0x345e` ("Hypex Electronics BV")
- **USB PID:** `0x03e8`
- **Product string:** `DSP3-213` (the FA503 identifies as the underlying DSP3
  module, not "FA503")
- **Serial number** (unit-specific): `0303A:H05U003268-0500M6579-0052`
- **HID interface:** vendor-defined collection on usage page `0xff00`,
  unnumbered report (no Report ID).

## Protocol notes — what we confirmed empirically

The PDF's reverse-engineered structure is right, with one transport quirk
specific to using Python's `hidapi` library:

- **No Report ID.** Every `dev.write()` must be prefixed with a `0x00`
  sentinel byte; hidapi strips it and sends the 64-byte payload raw on the
  interrupt OUT endpoint. Without the leading `0x00`, the first payload
  byte gets eaten as a phantom report ID and the amp returns a 64-byte all
  zero packet.
- **Feature reports do not work.** Both `get_feature_report` and
  `send_feature_report` fail (`read error` / `sent -1`). All control goes
  via the interrupt OUT/IN endpoints.
- **Reads.** `dev.read(64, timeout_ms=...)` returns the 64-byte payload
  directly (no Report ID prefix to strip).
- **Get-status request:** payload `06 02 00 00 ...` (rest zero-padded to 64).
- **Set State request:** payload `05 <input> <preset> <vol_lo> <vol_hi> 00 <mute>` ...
- Volume encoding confirmed: `int16 LE`, `dB * 100`. Example `0x60 0xf0` LE
  decodes as `0xf060` signed = `-4000` = `-40.00 dB`.

### Status response decoded

Example get-status response at preset 1 / -40 dB / unmuted:

```
05 06 01 60 f0 00 00 00 0f 19 00 02 16 01 04 00 00 00 00 00 00 60 f0 00
ff ff 01 00 00 00 00 00 00 00 00 f6 01 00 00 00 00 00 00 00 e0 01 99 03
16 00 06 01 60 f0 00 26 26 00 00 42 40 80 da 02
```

| Byte(s) | Field | Notes |
| --- | --- | --- |
| 0 | Response type | Always `0x05` so far |
| 1 | Packet ID | Echoes the request type (`0x06` for get-status, `0x00` for Set State response) |
| 2 | Current preset | 1-3 |
| 3-4 | Current volume | `int16 LE`, dB × 100 |
| 6 | Status flags | bit 7 = mute |
| 21-22 | Volume mirror | Same value as bytes 3-4 |
| 50 | **Active input source** | `0x06` = OPT, `0x04` = SPDIF, etc. (see PDF) - useful for display |
| 51 | Preset mirror | |
| 52-53 | Volume mirror (again) | |
| 56-57 | State-change counter | Increments by ~1 each command; useful for debouncing reads |
| 60 | Unknown | Changes with state |
| Tail | Unknown | Likely DSP/protection status |

The active input source at byte 50 is a freebie not documented in the PDF -
it means the knob can show the actual selected input on its display without
having to track it independently.

### Input source enum (from PDF)

| Value | Input |
| --- | --- |
| `0x00` | SCAN / no change (use this in writes when you don't want to change input) |
| `0x01` | XLR |
| `0x02` | RCA |
| `0x04` | SPDIF (coaxial) |
| `0x05` | AES |
| `0x06` | OPT (optical Toslink) |

## Scripts

All in `knob/software/tools/` and run from the repo-root venv at `.venv/`.
Install once: `.venv\Scripts\pip install hidapi`.

- **`hypex_probe.py`** - main probe. Subcommands: `list`, `status`,
  `set-volume <dB>`, `set-preset <1-3>`, `mute`, `unmute`, `interactive`.
  Read-only operations are safe; write operations round-trip current state
  for any field not explicitly set.
- **`hypex_diag.py`** - diagnostic that exercises multiple HID transfer
  styles (write/read, feature reports, bare read) to confirm which one the
  amp accepts. The dangerous all-zero Set State writes that originally
  caused the May-02 incident are now commented out.
- **`hypex_drain.py`** - reads any queued IN packets out of the amp's
  buffer, then does a clean get-status. Useful after an interrupted
  session.
- **`hypex_recover.py`** - one-shot: forces preset 1, -60 dB, mute ON.
  Run this if the amp ends up in an unsafe state.
- **`hypex_unmute_opt.py`** - one-shot: switches input to OPT and unmutes,
  preserving the current preset/volume.
- **`hypex_slider.py`** - Tkinter horizontal-slider GUI for live volume
  control with mute and refresh buttons. ~30 ms debounce on slide events.
- **`experiment_explore.py`** - read-mostly protocol exploration. Reads
  filter-name, get-reports `06 01` and `06 03`, and captures status across
  volume/mute/preset state matrices. Lowers volume to -50 dB before
  state-change captures, restores baseline on exit.
- **`experiment_06_03.py`** - focused decode of get-report `06 03`.
  Captures 25 responses across mute on/off and varied output levels;
  enforces a `-30 dB` volume cap. Writes JSON to
  `knob/docs/experiment_results/`.
- **`experiment_probe_reports.py`** - sweeps `(a, b)` opcode space with
  read-only intent. **HISTORICAL HAZARD: the version that ran on
  2026-05-02 included opcode `0x09` in its sweep, which knocked the amp
  offline. The next revision must restrict to confirmed-safe opcodes
  only (`0x03`, `0x04`, `0x06`, `0x08`).**

## Experiment log

### 2026-05-02 - First contact, protocol confirmed, knob direction set

Goals for the session:

1. Decide whether USB HID is the right transport for the knob.
2. Confirm the reverse-engineered protocol against a real FA503.
3. Demonstrate live read+write control.

What we did:

1. **Survey.** Read the local `hypex_fusion_notes.md`, the vendored Hypex
   USB HID PDF, and searched the web. Found
   [Turbopsych/UsbAmpControl](https://github.com/Turbopsych/UsbAmpControl)
   (MIT, ESP32-S3, tested on FA253 firmware v5.7) as the primary reference
   implementation. Confirmed the OLED display uses an undocumented "UART
   like" interface that Hypex declined to publish, so OLED-style protocols
   are not a viable shortcut.
2. **Decision.** USB HID is the only documented transport that exposes
   bidirectional state (volume readback in particular). Going with it.
3. **Wrote `hypex_probe.py`** - first cut of the Python probe targeting
   hidapi.
4. **Hit the report-ID bug.** First `status` returned 64 zero bytes. Wrote
   `hypex_diag.py` to test multiple transfer styles. Found that
   `dev.write([0x06, 0x02, ...])` was being interpreted by hidapi as
   "report ID 0x06, payload starting at 0x02", which the amp rejected
   silently. Adding a leading `0x00` (no-report-ID sentinel) fixed it.
5. **Successful first read:** `05 06 01 60 f0 00 00 00 ...` =
   preset 1, -40.00 dB, unmuted. PDF protocol confirmed.
6. **Incident.** The diagnostic's last test-case sent a Set State command
   with all zeros (`00 05 00 00 00 00 00 00 ...`) just to check whether
   that opcode triggered an OUT/IN exchange. It did - and the amp
   committed the zeros, dropping preset to 0 and volume to 0 dB. (User had
   physical access to the power button as a safety net.) Saved this as
   memory `feedback_hypex_set_state_safety.md`. **Lesson: Set State is
   atomic and destructive; never use it as a transport probe.** The
   diagnostic has been patched to no longer perform this write.
7. **Recovery.** Wrote `hypex_recover.py` and ran it. State went to
   preset 1, -60 dB, mute ON. Confirmed by re-reading status.
8. **Live control.** With user direction:
   - Switched input to OPT and unmuted (`hypex_unmute_opt.py`); the amp
     reported it had auto-unmuted already, possibly on SPDIF lock.
   - Volume sequence: -60 -> -50 -> -40 -> -30 dB via
     `hypex_probe.py set-volume`.
   - Mute / unmute round-trip via `hypex_probe.py mute` / `unmute`. Volume
     preserved across mute (-30 dB held).
9. **Spotted bonus field.** Decoded byte 50 of the status response as the
   active input source (`0x06` = OPT seen post-switch). Means the knob can
   display the active input without tracking state independently.

End-of-session state: working closed-loop control of a single FA503 from
Python on Windows. Volume / preset / mute / input all writable; volume /
preset / mute / input all readable.

### 2026-05-02 (later) - Tkinter slider UI

Built `knob/software/tools/hypex_slider.py`: a horizontal Tkinter slider
(-80 to 0 dB), a big dB readout, mute/refresh buttons, and a small status
line showing preset / input / mute. Slide events are debounced ~30 ms so a
fast drag coalesces into one write per ~30 ms idle rather than one write
per pixel of movement. `input_source` byte is sent as `0x00` (no change)
on every write to avoid accidentally re-routing. Confirmed working - real
time volume control feels immediate and the amp keeps up.

### 2026-05-02 (overnight) - Targeted decode + report-ID sweep + INCIDENT

User went to bed and left music streaming on SPDIF at -36 dB with a
volume cap of -30 dB and a no-destructive-changes constraint. Plan was to
decode `06 03`, sweep undocumented report IDs, find live-data sources,
then dig into status bytes 46-48 / 60.

#### Decode of `06 03` — null result, but informative

`experiment_06_03.py` captured 25 responses across:
- 10 back-to-back captures at the current baseline,
- 5 captures with mute ON at -50 dB,
- 5 captures with mute OFF at -50 dB,
- 5 captures with mute OFF at -30 dB.

**Every single byte across all 25 captures was identical.** So `06 03`
is a static configuration block, not a live meter or temperature feed.
The triple-repetition patterns we saw earlier (`28 28 28`, `14 14 14`,
`57 ec ff 00 x3`, etc.) are per-channel constants - probably calibration
values, fuse/limit settings, or a hardware identification block. They
don't change with audio activity at second-scale timing.

Slow-timescale changes (e.g. temperature drift over hours) were not
tested overnight - to be checked once we have a long-running poller and
a known-warm amp.

JSON: `knob/docs/experiment_results/exp_06_03_20260502T233309.json`.

#### Anomaly: baseline returned `preset=3`

The previous experiment had logged "Restored: preset=1" but the next
session's first read showed `preset=3`. Plausible cause: the
`experiment_explore.py` restore command was silently rate-limited (as
predicted by the rate-limit hypothesis), and the printed "preset=1"
came from a stale read. **Lesson for the firmware: every restore must
read back and verify, retrying with a small gap on mismatch.** The
overnight scripts now do this implicitly because they capture status
again after the final restore.

#### Report-ID sweep -- new finding: `08 NN` carries live counters

`experiment_probe_reports.py` swept opcode space `(a, b)` for `a` in
`{0x01..0x0b}` and `b` in `0x00..0x0f`, skipping `0x05` (Set State).

Non-trivial responses found:

| opcode | response head | notes |
|--------|---------------|-------|
| `03 NN` (any NN) | `03 00 00 00 ...` | Same template; the `03 08` "filter name" path that worked earlier returned an empty payload this time. May depend on amp state. |
| `04 NN` (any NN) | `04 01 00 00 ...` | Constant; possibly a "ping" / capability ack. |
| `06 01` | `06 16 05 01 e2 03 0f 0f 0f 20 20 20 ...` | Already known; per-channel triplets. |
| `06 02` | `05 00 03 ea f1 ...` | Standard get-status response. (Notable: `preset=3`, `vol=-36.06 dB`.) |
| `06 03` | `66 00 ...` | Static config (see above). |
| `08 NN` | `08 ?? f1 1a 00 00 a4 19 00 00 08 00 ...` | **LIVE** - bytes 2-3 and 6-7 are little-endian counters that incremented mid-sweep (`0x1af1 -> 0x1af2`, `0x19a4 -> 0x19a5`). Bytes 0,1, 8-15 have a fixed structure. |

This is the breakthrough: **`08 NN` returns live counters**. They look
like free-running timers or sample/frame counters running at slightly
different rates (both incremented by 1 during the sweep - same probe
index in both cases - so they may be tied). Still need a focused
fluctuation experiment to nail down their cadence and units, but this
is the right place to look for "is audio actually playing" or
"sample-rate lock" status.

JSON: `knob/docs/experiment_results/probe_reports_*.json` (partial -
script crashed before the final write; raw responses are in this log
above).

#### INCIDENT: opcode `0x09` hung the amp's USB

After successfully probing `08 0f`, the script moved to `09 00 00 00` -
the very next opcode family. The read on `09 00` raised `OSError: read
error` from hidapi, and the script crashed. Subsequent enumeration:

- t=0..30 s: `hypex_present=False` (15 polls)
- t=30..120 s: `hypex_present=False` (45 polls)

**The FA503 vanished from USB enumeration entirely** and did not return
within ~2 minutes. The host did not crash; other USB devices were fine.
Best interpretation: opcode `0x09` is an undocumented entry point -
likely a firmware-update / bootloader / DSP-reset path - and either the
amp's main MCU went into a state that doesn't expose the HID interface,
or its USB stack hung outright. Audio over SPDIF will probably be down
too because the DSP path is on the same MCU.

**Recovery procedure:**

1. Power-cycle the FA503 at the mains. Wait ~10 s before re-applying
   power.
2. Run `python knob/software/tools/hypex_probe.py list` and confirm
   VID `0x345e` reappears with product string `DSP3-213`.
3. Run `python knob/software/tools/hypex_probe.py status` and confirm
   the response decodes cleanly (preset / volume / mute fields look
   sensible). Note that the post-power-cycle preset/volume may be
   whatever the amp persists as default.
4. Optionally restore your listening state with the slider:
   `python knob/software/tools/hypex_slider.py`.

**Saved as feedback memory** (`feedback_hypex_dangerous_opcodes.md`) so
no future session repeats this.

#### Updated probe-safety policy

For the rest of the overnight session I have **stopped all USB
experiments** because the amp is unreachable. When experiments resume
(post power-cycle):

- Allowed read opcodes: `0x03 NN`, `0x04 NN`, `0x06 NN`, `0x08 NN`. All
  observed safe and read-only.
- Allowed write opcode: `0x05` (Set State), with field round-tripping
  and the `-30 dB` volume cap.
- **Never send**: `0x07 NN`, `0x09 NN`, `0x0a NN`, `0x0b NN`, or any
  higher first-byte. Untested and now known to risk hanging the amp.
- Probe new opcode families only with the user present and a clear
  recovery plan agreed in advance.

#### Carry-over next-experiment shopping list

Once the amp is back:

1. **Decode `08 NN`.** Highest priority. Capture 100 responses over
   ~10 s, plot the counters, calculate increment rate. Confirm whether
   they freeze when audio stops (true sample-rate lock indicator) or
   keep ticking regardless (free-running clock).
2. **Re-test `03 08` filter-name read** with the music known-on. The
   empty response we got tonight may have been timing/state related.
3. **Status byte 46-48 / 60 fine-grained captures** with audio
   on / off / muted / silent-source.
4. **Set-State rate limit measurement** - send same volume back-to-back
   with delays from 5 to 100 ms in 5 ms steps, find the floor.

### 2026-05-03 (later) - HFD binary inspection: what the amp actually exposes

User asked whether we have enough info to build a VU meter, and to look
at HFD.exe to learn more. Approach: HFD is a 18 MB Win32 Delphi /
FireMonkey binary at `C:\Program Files (x86)\Hypex Software\Hypex
filter design 5.2.4.24\HFD.exe`. It bundles `hidapi.dll` (same library
we use) and 32-bit. Decompiling Delphi to source is hard, but the
Delphi RTTI / VCL / FMX framework leaves *thousands* of symbol-style
strings in the binary (form names, property names, enum value names),
and that's enough to reconstruct the protocol's surface area.

Helper script: `knob/software/tools/_hfd_strings.py` — extracts ASCII
and UTF-16LE strings, then greps with case-insensitive patterns.
Re-run it any time we need to look up another HFD feature.

#### The amp exposes far more telemetry than we'd guessed

**Status bits** (named flags in HFD; almost certainly a single bitfield
the amp returns over USB):

```
AmpEnabled           deviceJustBooted     passwordMatchedHfd
prevButtonPressed    signalDetected       updateFiltersBusy
switchDSPOff         testsEnabled         signalPending
signalLostPending    StandbyPending       SystemUnhealty
SignalDetectedChanged
```

**Health bits** (errors and warnings, separate bitfield):

```
TempSensorFailError       TempTooHighError       VauxOutOfRangeError
DCErrorError              SRCI2CError            PowerFailureError
I2CbusError               TemperatureWarning
AnalogeClipWarning        DigitalClipWarning
```

**Per-input signal detection** (separate flags):

```
rcaSignalDetected         xlrSignalDetected
dspSPDIFSignalDetected    srcSignalDetected
```

**Per-channel temperature**: `Temperature 1`, `Temperature 2` (FA503
has 2 sensors; DSP3-224 has 4).

**Per-channel input AND output peak meters**: `LblDspPeakOutCh1`..`Ch4`,
`LblDspPeakLEFTIn`, `LblDspPeakRIGHTIn`. So the amp returns BOTH input
peak (L/R after the SRC) and output peak (per amp channel, post-DSP,
pre-power-stage).

**Other useful properties surfaced by RTTI:**

```
FusionAmpActualVolume         FusionAmpDSPPeakSetpoint
FusionAmpDSPPeakInput         FusionAmpDSPPeakInputs
FusionAmpDSPPeakOutput        FusionAmpDSPPeakOutputs
FusionAmpSignalDetected       FusionAmpSignalLostDetected
FusionAmpHealth               FusionAmpForceInput
FusionAmpEnableMFB            FusionAmpDetectedHardware
FusionAmpThirdChannelEnabled  FusionAmpFanConnected
FusionAmpStereoDSP            FusionAmpMixerPreset
FusionAmpTemperatureSetpoint  FusionAmpLimiterSettings
FusionAmpPasswordProtected    FusionAmpPasswordMatched
FusionAmpEeprom               FusionAmpPresetSettings
```

`acdInputMonitor`, `acdInputGain`, `acdInputMeter`, `acdOutputGain`,
`acdOutputMeter` look like enum members of an "amp control data" enum;
the integer values would be the report-ID sub-codes but those aren't
stored as strings.

#### What this means for the knob

- The amp tracks **all** of: per-input signal presence, signal-just-
  arrived / signal-just-lost edges, per-channel input + output peak
  meters, per-channel temperature, analog and digital clip warnings,
  pending-standby flag, "device just booted" flag, and a system-
  unhealthy flag.
- A "rich" knob — VU bar + clip indicator + temperature warning + idle
  dim on standby pending — is fully supported by the amp's firmware.
  We are not bottlenecked on the amp.
- We are bottlenecked on **knowing the report ID** for each of these.
  HFD knows; the strings tell us *what* exists, not *how* to ask for it.

#### Status confirmation: HFD release notes

Line 3 of `Documentation/Releasenotes hfd v5.2.4.txt`:
> "USB connection sometimes 'hanged' using VU meter, this is fixed"

Confirms VU meter is a USB-driven feature on the FA503.

#### Other findings worth noting

- HFD bundles **firmware HEX files** for the FA503 in
  `DSP3 firmware/`: v1.51 (PIC18, the ancient one) and v5.82 (PIC32,
  the current one we just re-flashed onto the user's amp). Strings in
  the firmware HEX are essentially nil ("WINUSB" and a hex digit
  table), so reverse-engineering the protocol from firmware is not
  practical without a PIC32 disassembler. HFD.exe is the better target.
- HFD uses `hid_write` / `hid_read` / `hid_read_timeout` AND
  `SendFeatureReport` / `GetFeatureReport`. Our diagnostic showed the
  control protocol uses interrupt OUT/IN only; feature reports
  presumably come into play during firmware update / project upload.
- HFD has a "noise generator for channel detection" feature
  (firmware v5.81 release notes). The amp can be told to inject a
  noise signal that rotates from channel to channel, at -55 dB. Useful
  during commissioning but not for the knob.

#### Path forward for VU metering — two options

1. **USB packet capture** (~5 minutes once set up). Install Wireshark +
   USBPcap on the Windows machine. With HFD running and the VU meter
   window open, capture USB traffic for ~5 seconds; the meter polls
   regularly so we'll see the exact OUT bytes HFD sends and the IN
   bytes it gets back. From that we can infer every read-side report
   ID for every property HFD shows. **By far the cheapest path to a
   complete spec.** Not destructive, not risky.
2. **Disassemble HFD.exe** with Ghidra or IDA. Slow, much harder for a
   Delphi binary, gives us the same answer we'd get from option 1 plus
   the write-side commands (filter coefficient writes etc.).

Recommendation: option 1 when the user has appetite for it. Until
then, the single status response we already decode covers the knob's
core needs (volume, preset, mute, input).



The amp came back. Recovery sequence that actually worked:

1. **Mains power cycle** (longer than ~30 s off this time). Amp
   re-enumerated on USB — same VID/PID/product string `DSP3-213`, but
   **the USB serial-number string was missing** (`None` from
   `hid.enumerate`, vs the previous `0303A:H05U003268-…`). That's the
   bootloader fingerprint per the HFD release notes.
2. **A safe `06 02` get-status read worked normally** — preset 3, but
   `volume = 0 dB` and `mute = ON`. Volume had been reset to factory
   default. (Mute being on was the only thing standing between the
   speakers and full output. Lucky.)
3. **Defensive write** dropped volume to -60 dB while preserving mute
   ON, preset 3, input OPT.
4. **Probes confirmed the DSP project was wiped:** `03 08` filter-name
   read returned empty (had been `'Config.xml'`); `06 03` and `06 01`
   blocks were unchanged (they're hardware-fixed, not project-tied);
   `08 00` byte 10 had incremented by 1 (looks like a boot counter).
5. **First attempt to use HFD failed** — HFD didn't see the amp.
6. **Amp went into standby** within minutes of being powered on without
   a project loaded — likely an "unconfigured" auto-shutdown path.
7. **Second power cycle, then USB cable unplug and replug** got the
   amp back into a state HFD could see.
8. **HFD prompted for a firmware update; user accepted.** Latest
   firmware re-flashed cleanly.
9. **Project re-uploaded** from
   `speakers/dsp/presets/KingRO4Y_MKIII_Presets/Config.xml`. Amp is
   back to normal operation.

#### Recovery procedure (canonical) — save this

If the amp ever goes silent / drops off USB / shows "P3" or any
flashing indicator with no audio:

1. **Mains off for ≥30 s.** Don't be impatient — capacitor discharge
   matters; quick toggles fail.
2. Power on. Wait 20-30 s before declaring the USB stack dead.
3. If it shows up on USB but HFD can't see it, **unplug and re-plug
   the USB cable** to force Windows to re-enumerate the HID interface.
4. Open HFD. If it offers a firmware update, **accept it** — the
   re-flash restores any corrupted firmware regions.
5. Open the project file at
   `speakers/dsp/presets/KingRO4Y_MKIII_Presets/Config.xml` and upload
   it.
6. Volume will likely come back at factory default (high). Drop it
   with the slider before unmuting. Use
   `python knob/software/tools/hypex_keepalive.py` if you want to keep
   the amp out of standby while you work in HFD.

If the amp drops off USB and *does not* re-enumerate after ≥2 minutes
on a clean power cycle, downgrade to **HFD 4.97** (community-known
workaround for stuck-bootloader recovery on Windows). If that also
fails, email **support@hypex.nl** describing the symptom; they can
re-flash the PIC manually.

#### Safe-Opcode Policy — read this before any new probe

The 0x09 incident was avoidable. Going forward:

| First byte | Status | Notes |
|------------|--------|-------|
| `0x03` | **safe (read-only)** | Returns name strings; only `0x03 0x08` known to carry payload |
| `0x04` | **safe (read-only)** | Returns constant `04 01 …`; possibly a "ping" |
| `0x05` | **destructive write** | Set State; round-trip every field, never send speculatively, see `feedback_hypex_set_state_safety.md` |
| `0x06` | **safe (read-only)** | Get Reports; `0x06 0x01`, `0x06 0x02`, `0x06 0x03` documented; other sub-codes unused |
| `0x07` | **DO NOT SEND** | Untested. Adjacent to known-dangerous 0x09. |
| `0x08` | **safe (read-only)** | Returns live counters; the report we want for sample-lock detection |
| `0x09` | **DO NOT SEND, EVER** | Hung the amp's USB on 2026-05-02. Required power cycle + firmware re-flash + project re-upload. |
| `0x0a`..`0x0f` | **DO NOT SEND** | Untested; assume same risk class as 0x09 until proven otherwise |
| `0x10`+ | **DO NOT SEND** | Untested. |

Operational rules:

1. **No autonomous probing of unknown opcodes when the user is asleep
   or away.** New first-byte opcodes are user-present-only.
2. **Always have an exit plan before each probe.** Specifically: a
   power switch the user can hit, and an agreed-upon "stop and
   document" trigger.
3. **Probe scripts must list their allowed opcodes explicitly** and
   refuse to send anything outside the list. The current
   `experiment_probe_reports.py` is restricted to `{0x03, 0x04, 0x06,
   0x08}`. Don't widen this without doing a controlled probe of one
   new value at a time, with the user watching.
4. **Set State writes always round-trip current state** (the
   `set_state(...)` helper in `hypex_probe.py` does this; reuse it
   rather than rolling your own).
5. **Volume cap during exploration:** -30 dB unless the user
   explicitly authorises higher.
6. **After any write, verify with a fresh `06 02` read** — don't trust
   the immediate response packet alone (the May-02 "preset=1 restored"
   message turned out to be stale buffer; the actual state was preset
   3).

We were lucky this time: the bootloader recovery path worked and the
hardware had no permanent damage. A different opcode in the 0x09 range
might have been a programming command that bricks the amp's MCU
outright, requiring vendor RMA. The cost-of-being-wrong on opcode
probes is "send the amp back to the Netherlands". That is the wrong
risk to take for incremental knob features.



Driven by `knob/software/tools/experiment_explore.py`. Read-only commands
were tried first; state-change captures lowered volume to -50 dB before
sweeping and restored the original (preset 1 / -36 dB / unmuted) at the
end.

#### Filter name request (`03 08 00 00`)

Documented in the PDF; confirmed working. **Returns the project filename,
not a friendly name** - in our case `'Config.xml'`, an ASCII run starting
at byte 2 of the response. So the string is whatever the user named the
HFD project, which for our preset directory
`speakers/dsp/presets/KingRO4Y_MKIII_Presets/` happens to be the literal
file name. Future implication: if the knob wants to show preset names, we
either need to rename the HFD project file to something user-friendly, or
the per-preset DSP block label has to come from a different report we
haven't found yet.

Raw response:

```
03 00 43 6f 6e 66 69 67 2e 78 6d 6c 00 ...
   ^- ASCII: "Config.xml"
```

#### Get Report `06 01` (configuration / capabilities block)

Fully readable. 64 bytes of mostly-structured data:

```
06 16 05 01 e2 03 0f 0f 0f 20 20 20 03 05 81 05
00 07 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 94 11 dc 05 dc 05 dc 05 00 00 00 00 00 00
```

Triple-repetition patterns (`0f 0f 0f`, `20 20 20`, `dc 05 dc 05 dc 05`)
strongly suggest **per-channel data** - the FA503 has three channels
(2x500 W woofer/mid + 1x100 W tweeter), so each triplet is one value per
channel. `dc 05` = 0x05dc = 1500 in three slots. Could be max output
power, voltage rail headroom, or per-channel limit. Worth pinning down
with a follow-up experiment. The `e2 03` early in the packet decodes to
`0x03e2` = 994 (no obvious meaning) and `94 11` at byte 50-51 decodes to
`0x1194` = 4500.

#### Get Report `06 03` (status / sensor / runtime block)

Different response-type byte (`0x66` rather than `0x05`).

```
66 00 00 00 00 00 00 00 00 00 05 09 16 01 01 41
00 10 00 00 1f 85 ab 02 1f 85 ab 02 29 5c 0f 05
57 ec ff 00 57 ec ff 00 57 ec ff 00 b9 fc ff 00
b9 fc ff 00 b9 fc ff 00 28 28 28 14 14 14 00 00
```

More triple-repetition patterns - `57 ec ff 00` x3, `b9 fc ff 00` x3,
`28 28 28`, `14 14 14`. These look like **per-channel 24- or 32-bit values
plus per-channel byte readings** (28 = 40, 14 = 20 - consistent with
temperature in degrees C, or with current draw in tenths of an amp). The
twin `1f 85 ab 02` blocks (=`0x02ab851f` LE = 44,729,119) at bytes 20-23
and 24-27 are probably uptime in seconds or a calibration constant.

This is the most interesting report we've found - it appears to expose
metering and health data that the PDF does not document. Decoding it
properly is the highest-value next experiment.

#### Status-tail captures (sweeping volume / mute / preset)

| byte | v=-60 | v=-50 | v=-40* | mute=ON | mute=OFF | preset1 | preset2 | preset3 |
|------|------|------|------|------|------|------|------|------|
|  2 | `01` | `01` | `01`  | `01` | `01` | `01` | `02` | `03` |
|  3 | `90` | `78` | `78`* | `78` | `78` | `78` | `78` | `78` |
|  4 | `e8` | `ec` | `ec`* | `ec` | `ec` | `ec` | `ec` | `ec` |
|  6 | `00` | `00` | `00`  | `80` | `00` | `00` | `00` | `00` |
| 46 | `d7` | `d4` | `d4`  | `17` | `9c` | `9c` | `8e` | `8e` |
| 47 | `01` | `01` | `01`  | `01` | `02` | `02` | `02` | `02` |
| 48 | `0c` | `04` | `04`  | `02` | `02` | `02` | `01` | `01` |
| 52 | `ea` | `ea` | `78`  | `78` | `78` | `78` | `78` | `78` |
| 53 | `f1` | `f1` | `ec`  | `ec` | `ec` | `ec` | `ec` | `ec` |
| 60 | `00` | `40` | `40`  | `00` | `40` | `40` | `00` | `00` |

`*` The `v=-40 dB` capture's actual readback was still `-50 dB` - the
amp silently ignored a Set State write that arrived ~50 ms after the
previous one. **Suspected rate-limiting** on consecutive Set State
commands. Slider works fine because the debounce throttle gives the amp
breathing room, but firmware will need a minimum gap (probably 20-50 ms)
between volume writes. Worth a dedicated experiment.

#### What we learned about specific bytes

- **Byte 6** confirmed: bit 7 = mute (matches PDF). Bits 0-6 stayed `0` in
  every capture, even across input/preset changes. Unused or reserved on
  this firmware.
- **Bytes 46-48** vary with every state change, but not monotonically.
  Reading them as a 24-bit LE unsigned integer gives no obvious pattern;
  the numbers do increase substantially when the amp goes from muted to
  unmuted at the same volume. **Hypothesis: a metering value** (input
  level, output level, or DSP rail) - not a counter, not a static config.
- **Bytes 52-53** are a *second copy of volume*, but they **lag the first
  copy by one or two captures** (i.e. they show the previous volume).
  Likely "current volume" vs "target volume" during a soft-ramp - bytes
  3-4 are the commanded value, bytes 52-53 are where the actual fader is
  right now. If true, this is exactly what the knob needs to draw a
  smooth volume-changing animation.
- **Byte 60** is `0x40` only when **preset 1 is active and the amp is
  unmuted**. Switching to preset 2 or 3 cleared it; muting cleared it.
  Most likely an **"audio output active"** flag - bit 6 set when the DSP
  is actively producing output. Presets 2 and 3 might have all-zero gain
  trims in our current config so the amp reports them as silent. Test:
  switch to preset 2, send actual audio, see if byte 60 goes back to
  0x40.
- **Bytes 21-22** (`60 f0` early in our session, then drifted) and the
  trailing block (`62 00/40 80 da 02`) didn't change cleanly across our
  state matrix. Need finer-grained experiments to label them.

## Open questions / next steps

In rough priority order:

1. **Decode `06 03` (sensor/runtime block).** Highest-value unknown -
   triple-repetition patterns suggest per-channel temperatures and meter
   values. Capture the response under controlled conditions: cold amp vs
   warm amp, signal vs no-signal, low vs high output level, each preset
   active. Look for monotonic changes and label fields.
2. **Confirm `06 01` field meanings.** Per-channel triplets imply
   capability/limit values. Compare response across the FA503 once we get
   to the second amp; values that stay identical are model constants,
   values that differ may be unit-specific (serial-tied calibration).
3. **Investigate Set State rate-limiting.** A v=-40 dB write ~50 ms after
   a v=-50 dB write was silently ignored. Find the minimum reliable gap
   and document it - the firmware will need it.
4. **Pin down byte 46-48 and byte 60.** Hypotheses: 46-48 = output meter,
   60 = audio-active flag. Test with: known signal level on OPT, switch
   between presets that have non-zero vs zero gain, run with no signal at
   all.
5. **Per-preset filter name.** The `03 08` report only returned the
   project filename. There must be a separate report (or per-preset
   request) that yields a friendlier per-preset string - HFD shows them.
   Look for an `03 NN` variant.
6. **Confirm master/slave behaviour.** Wire both FA503s in master/slave
   and verify that the slave follows the master's volume/preset/mute
   purely from a cable - so the knob really only needs one USB connection.
   If the slave does *not* follow over the chain link, we need a USB hub
   plus dual-host code in the firmware.
7. **Power on/off behaviour.** Investigate whether USB control survives
   the amp's own standby, and whether the knob can wake it.
8. **Firmware port.** Translate the working Python protocol layer to C on
   ESP32-S3 with TinyUSB Host (or ESP-IDF USB Host HID class) using
   UsbAmpControl as a reference. Single-amp first.
9. **Knob UX design.** Encoder + button + small display (SSD1306 or
   similar). Decide on the rotary feel (detents vs. continuous), tap/long-
   press behaviour, what the display shows by default, idle dimming.
10. **Hardware design.** PCB schematic and enclosure CAD - currently
    `knob/hardware/pcb/` and `knob/enclosure/cad/` are empty scaffolds.

## References

- Local notes: `hypex_fusion_notes.md` (repo root) - protocol summary
  distilled from the sources below.
- Local PDF: `speakers/vendor/hypex/Hypex USB Hid documentation.pdf` -
  reverse-engineered protocol spec.
- GitHub: [Turbopsych/UsbAmpControl](https://github.com/Turbopsych/UsbAmpControl) -
  MIT-licensed ESP32-S3 reference implementation (tested on FA253 fw 5.7).
- diyAudio thread: <https://www.diyaudio.com/community/threads/esp32-s3-based-fusion-amp-controller-with-ab-and-abx-testing-support.430952/>
- diyAudio thread on the OLED UART: <https://www.diyaudio.com/community/threads/hypex-fusion-remote-kit-and-led-display-how-do-they-look.421814/>
