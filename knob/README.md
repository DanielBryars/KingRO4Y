# The Knob

A wireless volume knob for the KingRO4Y speakers (two Hypex FA503 plate
amps). The knob is deliberately amp-agnostic: it talks BLE to small
**dongles**, one plugged into each amp's USB port, and the dongles
translate generic volume/preset/mute/input commands into the vendor
protocol. New amp = new dongle, same knob.

Full design: [`docs/Architecture.md`](docs/Architecture.md). Running lab
notebook: [`docs/experiments.md`](docs/experiments.md). Wire protocol:
[`docs/HypexUsbProtocol.md`](docs/HypexUsbProtocol.md).

> **Before sending anything to the amp, read the Safe-Opcode Policy in
> `docs/experiments.md`.** Opcode 0x09 once knocked the amp off USB and
> required a firmware re-flash. The allowlist exists for a reason.

## Status (2026-07-06)

| Piece | State |
|-------|-------|
| Hypex USB protocol | Reverse-engineered and verified on the real FA503; VU-meter polling decoded from an HFD packet capture. Python tools in `software/tools/`. |
| Dongle firmware, BLE half | **Working.** ESP-IDF project in `firmware/dongle/`; pairs with a Fosi Audio VOL20 BLE knob and drives a simulated amp. Runs on an Olimex ESP32-DevKit-LiPo. |
| Dongle firmware, USB half | Next. Needs the ESP32-S3-USB-OTG board (on order); raw `usb_host` interrupt transfers per `firmware/dongle/README.md` phase B notes. |
| Knob hardware (nRF52840 + OLED + encoder) | Designed on paper, not started. |
| PCB / enclosure | Empty scaffolds. |

## Layout

- `docs/` — architecture, protocol spec, experiment log + raw captures
- `firmware/dongle/` — ESP-IDF dongle firmware (phase A: BLE half)
- `software/tools/` — Python probes/utilities for the amp (Windows, hidapi)
- `vendor/` — vendored third-party reference code (Espressif example)
- `hardware/`, `enclosure/` — future PCB and CAD
