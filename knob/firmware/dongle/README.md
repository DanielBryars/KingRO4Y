# KingRO4Y dongle firmware

The dongle plugs into a Hypex FA503's USB port (USB **Mini-B** on the amp)
and bridges it to Bluetooth. Two halves, kept strictly apart:

- **BLE half** (`components/ble_input`) — BLE HID host. Pairs with a BLE
  volume controller (bring-up device: Fosi Audio VOL20), decodes its
  Consumer Control reports into `input_event_t`s.
- **USB half** (`main/amp_backend*`) — drives the amp. Phase A ships a
  *simulated* backend (`amp_backend_log.c`); phase B replaces it with the
  ESP32-S3 USB-host implementation behind the same `amp_backend.h`
  interface.

They meet only in `main/input_map.c`.

Protocol logic lives in `components/hypex_proto` — a dependency-free C port
of `knob/software/tools/hypex_probe.py`, unit-tested on the host against
real packet captures. See `knob/docs/HypexUsbProtocol.md` and the
Safe-Opcode Policy in `knob/docs/experiments.md` before touching anything
that talks to the amp.

## Phases and boards

| Phase | Board | What works |
|-------|-------|-----------|
| A (this code) | Olimex ESP32-DevKit-LiPo (WROOM-32E) — any BLE-capable ESP32 | VOL20 pairs; turning it updates a simulated amp on the serial console |
| B | ESP32-S3 (recommended: **ESP32-S3-USB-OTG** — its USB-A host port supplies 5 V) | Same, but driving the real FA503 over USB |

The plain ESP32 has **no USB hardware**, which is why phase A simulates the
amp. Everything except the USB backend carries over to the S3 unchanged.

## Build & flash (phase A)

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
v5.5.x (v5.4 should also work). Easiest install on Windows: the "ESP-IDF"
VS Code extension, or the ESP-IDF Windows installer.

```
cd knob/firmware/dongle
idf.py set-target esp32
idf.py -p COMx flash monitor
```

Then put the VOL20 in pairing mode: it must be **forgotten by / disconnected
from any previously paired device first** (it bonds to one host at a time —
double-click its light-mode button to disconnect), power it on, LED flashing
blue. The console shows the scan, connection, every raw input report, and
the simulated amp state.

Phase A bring-up status (2026-07-06, on the Olimex):

1. ~~VOL20 appears in the BLE scan~~ **done** — needed a scan-filter fix
   (it advertises HID *appearance* 0x03C1, not the 0x1812 service UUID).
2. ~~Connection + bonding~~ **done** — "Just Works" bonding on first
   attempt; auto-reconnects after a dongle reset; battery reports ~9 s.
3. ~~Raw report capture + decoder~~ **done** — it's a 3-byte *bitmap*
   report (see `decode_vol20_bits()` and
   `knob/docs/experiment_results/vol20_ble_reports_20260706.log`).
4. Rotation moves the simulated volume — decoder in place, final visual
   check pending, plus a choreographed gesture test to pin the rotation
   direction and the button bits (steps in the decoder's comment).

## Host tests

`components/hypex_proto` is plain C99 with no ESP-IDF dependency:

```
cd host_tests
python run_tests.py     # finds gcc/clang/tcc/MSVC; builds & runs
```

Golden vectors are real FA503 packets from the 2026-05 capture sessions.

## Phase B notes (ESP32-S3 USB host)

- Use the raw `usb_host` client API, **not** the `usb_host_hid` class
  driver — the class driver never uses the interrupt OUT endpoint the
  Hypex protocol needs. Raw 64-byte interrupt transfers to OUT `0x01` /
  IN `0x81`, matching [Turbopsych/UsbAmpControl](https://github.com/Turbopsych/UsbAmpControl)
  (MIT; good reference for enumeration/reconnect handling).
- Filter on VID `0x345e` / PID `0x03e8` (UsbAmpControl opens anything).
- Enforce ≥100 ms between Set State writes and verify every write with a
  fresh `0x06 0x02` read (both learned the hard way — see experiments.md).
- The amp will NOT power the dongle: the FA503's Mini-B port is a USB
  *device* port, so the dongle-as-host must supply 5 V VBUS itself. The
  ESP32-S3-USB-OTG board handles this (GPIO12 `DEV_VBUS_EN`); a DevKitC-1
  needs its VBUS diode bridged or external 5 V injection.

## Vendored reference

`knob/vendor/esp_hid_host_example/` is Espressif's esp_hid_host example
(release/v5.5, public domain); `components/ble_input/esp_hid_gap.[ch]` are
unmodified copies from it.
