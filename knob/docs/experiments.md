# Knob Experiments Log

A running log of work on the knob subproject. Append new sessions at the bottom.

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

## Open questions / next steps

In rough priority order:

1. **Confirm master/slave behaviour.** Wire both FA503s in master/slave
   and verify that the slave follows the master's volume/preset/mute
   purely from a cable - so the knob really only needs one USB connection.
   If the slave does *not* follow over the chain link, we need a USB hub
   plus dual-host code in the firmware.
2. **Fully decode the status tail.** Bytes 21-22, 35-36, 50-53, 56-57, 60,
   and the trailing block (`42 40 80 da 02`) carry useful information.
   Worth a short session diffing responses across known state changes
   (volume sweep, preset change, mute toggle, input change, no input
   signal, signal lock) to label them.
3. **Filter-name read.** The PDF documents a `03 08 00 00 ...` request
   that returns the loaded DSP filter name as ASCII. We haven't tested it.
   Useful for the knob display when switching presets.
4. **Power on/off behaviour.** Investigate whether USB control survives
   the amp's own standby, and whether the knob can wake it.
5. **Firmware port.** Translate the working Python protocol layer to C on
   ESP32-S3 with TinyUSB Host (or ESP-IDF USB Host HID class) using
   UsbAmpControl as a reference. Single-amp first.
6. **Knob UX design.** Encoder + button + small display (SSD1306 or
   similar). Decide on the rotary feel (detents vs. continuous), tap/long-
   press behaviour, what the display shows by default, idle dimming.
7. **Hardware design.** PCB schematic and enclosure CAD - currently
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
