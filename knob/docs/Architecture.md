# Knob System Architecture

Version 1, drafted 2026-05-03 from design discussions.

## Goal

Build a custom Bluetooth-controlled knob for the KingRO4Y speakers
(Hypex FA503 amps), and design it so it isn't tied to any specific
amp model. The knob never knows it's talking to a Hypex; it talks to
**dongles** which translate generic "set volume / set preset / mute /
choose input" commands into vendor-specific protocols. New speaker
models = new dongle, same knob.

## Non-goals (v1)

- **Live VU metering on the knob display.** The battery cost of a
  10 Hz BLE notify stream while the display is on is large, and the
  knob is supposed to be a small low-power object. Metering can be
  re-added later as a v2 feature with explicit user opt-in.
- **Bluetooth LE Audio (VCP / VCS).** The Volume Control Profile is
  designed for a much larger system (audio routing, hearing aids,
  broadcast streams) and the spec is correspondingly heavy. We don't
  need any of that. A small custom GATT service does everything we
  need with one-tenth the surface area.
- **Multi-room synchronisation, audio streaming, or any audio path
  responsibility.** The knob and dongle are pure *control plane*.
  Audio still flows through whatever the user has wired up (SPDIF,
  RCA, AES, etc.) into the FA503's existing inputs.

## High-level topology

```
   [Knob]  <-- BLE -->  [Dongle L]  <-- USB -->  [FA503 in left speaker]
                        [Dongle R]  <-- USB -->  [FA503 in right speaker]
                        [other dongles, sub, soundbar, ...]
```

Three discrete components:

1. **Knob** — battery-powered, BLE Central, encoder + display + button.
   Holds N simultaneous connections to dongles. User-facing UX lives
   here.
2. **Dongle** — wall-powered (from amp's USB), BLE Peripheral, USB
   Host. One per amp. Implements the vendor-specific control protocol
   on the USB side and exposes a generic GATT service on the BLE side.
3. **Amp** — unmodified Hypex FA503 (or, in principle, any amp with
   an analogous dongle).

Pairing model: knob bonds with each dongle once. After that the knob
auto-reconnects to known dongles when in range. A user "speaker group"
(typically the L+R dongles for one stereo pair) is a stored set of
peer addresses.

## Why this design

A direct-USB knob would be cheaper and have lower latency, but:

- **Wires.** A USB cable from knob to amp is the limiting factor on
  where you can put the knob. BLE removes that.
- **Lock-in.** A direct-USB knob would have to know the Hypex protocol
  in firmware. Any future amp change (different model, different make)
  means reflashing the knob. With a dongle, the knob is forever
  agnostic and only the dongle changes.
- **Multi-speaker.** The user has *two* FA503s. The Hypex digital
  chain carries audio between amps but **not control state** — so a
  stereo pair needs two control links no matter what. Two dongles +
  one knob is cleaner than one knob with a USB hub talking to two
  amps simultaneously.
- **Mix-and-match.** Once the dongle abstraction exists, the knob
  works with anything that has a dongle (or that supports our service
  natively). One knob can control a Hypex stereo pair, a Genelec sub,
  and an old AVR all from the same physical encoder.

The cost is one new product to design (the dongle) and BLE pairing
UX. Both are tractable.

## Knob design

### Hardware sketch

| Component | Choice | Rationale |
|-----------|--------|-----------|
| MCU | **nRF52840** | Best-in-class BLE power, Zephyr support, well-understood. ESP32-C6 is a fine alternative if Wi-Fi is wanted later. |
| Display | 1.3" SH1106 OLED (128×64) | Cheap, readable, can be put to sleep entirely when idle. |
| Encoder | Bourns PEC11R rotary + push | Industry standard quadrature with detents. |
| Optional 2nd switch | Soft button | "Mode" / long-press alternative if encoder press feels overloaded. |
| Battery | 300–500 mAh LiPo | Months of light use; rechargeable. |
| Charge | TP4056 + USB-C | Standard, cheap, robust. |
| Enclosure | TBD (CAD lives in `knob/enclosure/`) | Match the speaker aesthetic. |

### Firmware roles

The knob is a **BLE Central with multiple simultaneous Peripheral
connections**. nRF52840 supports up to 8 concurrent connections
which is plenty.

States:

```
boot
  └── scan-for-known-bonded-peers
        └── auto-connect to all in range (parallel)
              └── normal-operation
                    └── encoder turn -> dispatch to active group
                    └── encoder press -> menu
                    └── idle for N seconds -> display sleep
                    └── connection lost -> reconnect background
  └── pairing-mode (long press)
        └── BLE scan, list nearby unbonded dongles
        └── user picks, bond, save
        └── return to normal-operation
```

Active group selection:
- Default: "everything bonded and connected"
- Menu lets the user define groups (e.g. "Living Room" = L + R Hypex
  dongles; "Bedroom" = bedroom soundbar)
- Encoder press cycles between: all-paired / left-only / right-only
  / next-group

Dispatch fan-out: a single encoder turn emits a `Volume_dB` write to
every member of the active group. Writes are pipelined per peer
(don't wait for a write-response from peer A before sending to B).

### UX (v1)

- Idle: display dark.
- Touch / turn / press: display wakes, shows current volume of active
  group (read from any one connected peer; assume sync).
- Encoder turn: ±0.5 dB per detent.
- Encoder press once: cycle group (all / L / R / next preset / mute).
- Encoder long press: enter menu (pair new device, manage groups,
  delete bonds, factory reset).
- Display sleeps after 10 s of inactivity. Encoder wakes it.

### Power budget (rough)

Idle, BLE peripherals connected with 100 ms connection interval,
display off: ~30 µA average on nRF52840. 300 mAh LiPo → ~1 year of
standby.

Display on, polling at 10 Hz: ~5 mA. With 30 minutes of "active" use
per day + 23.5 h idle, average draw ~150 µA. ~80 days per charge.

These are back-of-envelope; real numbers depend heavily on connection
parameters, display refresh strategy, and encoder polling. Worth
profiling on first prototype.

## Dongle design

### Hardware sketch

| Component | Choice | Rationale |
|-----------|--------|-----------|
| MCU | **ESP32-S3** | Native USB OTG host + BLE 5.0 in one chip. Mature ESP-IDF support for both. |
| USB | USB-A male plug, ESP32-S3's OTG pin | Plug straight into amp. |
| Power | 5 V from amp's USB bus | Amp provides plenty for HID-only use. |
| Indicator | One small LED | Connected / paired / error states. |
| Enclosure | Heat-shrink or 3D-printed shell | Tiny — most of the volume is the USB-A plug. |

Form factor: USB-A flash-drive sized, single-piece. No buttons (pair
mode triggered automatically on first power-up; reset by a long
press of a recessed button or by a HFD-style hold-on-boot).

### Firmware

Two cooperating tasks:

1. **USB Host task** — speaks Hypex per `HypexUsbProtocol.md`.
   - Enumerates the FA503, opens the vendor HID interface (VID
     `0x345e` PID `0x03e8`).
   - Maintains a state model (current preset / volume / mute /
     input) cached locally.
   - Pushes state changes from BLE writes onto the USB side with
     proper rate limiting (≥80 ms gap between Set State writes).
   - Polls the amp at low rate (~1 Hz) with `0x06 0x02` to keep the
     state cache fresh in case HFD or another tool changed it.

2. **BLE Peripheral task** — exposes the custom GATT service.
   - Advertises with name `FA503-XXXXXX` where `XXXXXX` is the last
     6 chars of the amp's serial.
   - Accepts one Central connection at a time (the knob).
   - Handles writes by enqueueing to USB Host task; reflects state
     changes back via Notify on the relevant characteristic.

State synchronisation: the dongle is the source of truth for
"current amp state" since it can read the amp directly. Knob writes
are advisory; they're applied to the amp and the *resulting* state
(post-write read from amp) is what gets notified back. So if the
amp rejects a write (e.g. rate-limited) the knob sees the actual
state, not a phantom successful one.

### Bootstrap behaviour

- On power-up, USB enumerate the amp. If enumeration fails, blink
  LED red, retry.
- Once the amp is talking, advertise BLE.
- If no Central connects within 5 minutes after first power-up,
  drop into pairing-allowed mode (advertising with bondable flag).
- Once bonded once, only the bonded knob can connect; pairing-mode
  re-entered only via long-press of the dongle's reset button or
  a HFD-style hold-on-boot.

## BLE design

### Custom GATT service

Service UUID: a 128-bit custom UUID, e.g.
`KINGR04Y-AMPCTRL-V1` (encoded as a real UUID). Single service.

Characteristics:

| Characteristic | UUID Suffix | Type | Access | Meaning |
|----------------|-------------|------|--------|---------|
| `volume_db_x100` | 0x0001 | sint16 | R/W/N | Volume in dB × 100 (range −9900 to 0). Same encoding as the Hypex protocol so no scaling on the dongle. |
| `preset` | 0x0002 | uint8 | R/W/N | 1, 2, or 3 |
| `mute` | 0x0003 | uint8 | R/W/N | 0 = unmuted, 1 = muted |
| `input_source` | 0x0004 | uint8 | R/W/N | Same enum as Hypex (0x00 = scan/no-change, 0x01 XLR, 0x02 RCA, 0x04 SPDIF, 0x05 AES, 0x06 OPT) |
| `status_flags` | 0x0010 | uint16 | R/N | Bitfield: bit 0 = signal_detected, bit 1 = audio_active, bit 2 = standby_pending, bit 3 = system_unhealthy. (Filled in once we've decoded the status-bits report from HFD.) |
| `model_name` | 0x0080 | utf8 string | R | e.g. `"FA503"` |
| `serial_number` | 0x0081 | utf8 string | R | per-amp serial |
| `protocol_version` | 0x0082 | uint8 | R | 1 for v1; lets the knob future-proof |

Properties:
- All R/W characteristics support **Notify** so the knob gets
  out-of-band updates (e.g. user changed input via HFD).
- All writes are **Write Without Response** for fast fan-out, with
  **Write With Response** as a fallback the knob can use when it
  cares about delivery confirmation.
- Encryption with bonding (LE Secure Connections) required after
  pairing.

### Knob UX commands → BLE writes

| User action | BLE traffic |
|-------------|-------------|
| Encoder turn | `volume_db_x100.write(new_value)` to all peers in active group |
| Encoder press cycle to "L only" | (none — UI mode change only) |
| Encoder press to mute | `mute.write(1)` to all peers in active group |
| Long-press to enter menu | (none — UI mode) |
| Menu: "set preset 2" | `preset.write(2)` to all peers in active group |
| Boot / reconnect | Subscribe to all Notify characteristics, then `read()` each to populate display |

### Optional v2: HID-over-GATT for foreign devices

The knob can additionally expose **HID over GATT (HOGP)** as a BLE
Peripheral — making it look like a Bluetooth keyboard with Consumer
Control keys (`Volume+`, `Volume−`, `Mute`, `Play/Pause`). This lets
the same physical knob control:

- A phone you handed across the room (the phone scans, finds the
  knob, pairs).
- A soundbar / smart speaker that accepts BT remotes.
- A laptop, TV, or anything else with a BT host stack.

This is a **dual-role** design: knob is BLE Central toward our
dongles, BLE Peripheral toward foreign hosts. Both nRF52840 and
ESP32 stacks support dual-role concurrent operation. Out of v1 to
keep scope tight; worth designing the firmware structure to allow
adding it without a re-architecture.

## Pairing & bonding flow

1. **First-time setup of a dongle.** User plugs the dongle into the
   amp, powers the amp on. Dongle starts advertising in pair-mode.
2. **User puts the knob into pair mode.** Long-press encoder for 3 s
   from idle.
3. **Knob scans, lists discovered dongles** (by name + RSSI). User
   rotates encoder to highlight, presses to select.
4. **Bonding handshake.** LE Secure Connections; numeric comparison
   skipped (no display on dongle), so this is "Just Works" pairing.
   Acceptable for a home audio context; if higher security is
   desired, a passkey on the dongle's LED blink pattern could be
   added later.
5. **Bond stored** on both sides. From now on the knob auto-connects
   when in range.
6. **Repeat for each dongle** (typically 2 — left and right).

Removing a bond: knob menu → "Forget device" → pick → confirm. The
dongle re-enters pair-mode if its bond is forgotten and it doesn't
see the knob for a while.

## Multi-speaker / multi-make

Three modes the knob supports out of v1:

1. **Stereo pair** — two dongles (L, R) treated as one logical
   "device". Encoder turn writes to both simultaneously. Balance
   offset stored locally (default 0); long-press enters balance mode
   and the encoder shifts L vs R.
2. **Group** — user-defined named group of N peers, all controlled
   together.
3. **Individual** — encoder press cycles to "L only" or "R only" so
   you can adjust one side temporarily.

Different makes:
- A Genelec / Neumann / NAD amp would need its own dongle (ESP32-S3
  + the relevant control protocol). The knob doesn't change.
- A foreign device that natively speaks our service (unlikely but
  possible) is fully integrated.
- A foreign device that only speaks HID Consumer Control is reachable
  in v2 once the knob has the HID-over-GATT role.

## Build order

The dongle is the technical risk; the knob is well-understood
territory. Build in this order:

1. **Dongle MVP, USB side only.** ESP32-S3 dev board. Port the
   Python protocol layer in `knob/software/tools/hypex_*.py` to C
   for ESP-IDF USB Host HID. Verify volume / preset / mute / input
   work via a local test harness (LEDs, serial console).
2. **Dongle MVP, add BLE.** Implement the custom GATT service. Test
   with the **nRF Connect** app on a phone — write `volume_db_x100`
   and verify the amp responds. **This is the architecture milestone.**
3. **Knob MVP, hardware bring-up.** nRF52840 dev kit, encoder, OLED.
   Display "Hello world".
4. **Knob MVP, BLE Central.** Connect to a known dongle MAC, write
   volume on encoder turn. Verify in-the-wild behaviour.
5. **Knob MVP, full UX.** Pairing flow, multi-peer fan-out, menu,
   sleep / wake.
6. **PCB + enclosure for both.** Productionise.
7. **Stretch:** HID-over-GATT on knob; status flags decode and
   display; VU meter v2.

Each milestone is a few weekends.

## Risks and how to manage them

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| BLE write latency feels laggy when turning the encoder fast | Medium | UX | Use Write Without Response; pipeline; profile early. <50 ms end-to-end is achievable. |
| Knob battery life worse than projected | Medium | UX | Profile current draw with display strategies; keep display off by default, wake on encoder. |
| Dongle drops the BLE connection when the amp does something USB-stack-disrupting (e.g. firmware-update mode) | Low | Recovery | Dongle's BLE side keeps advertising "amp unreachable" status; knob shows offline indicator. Auto-reconnects when amp returns. |
| Hypex changes their USB protocol in a future firmware update | Low | Maintenance | Dongle firmware can be updated over BLE OTA; keep room in flash for two firmware images. |
| BLE pairing UX confuses users | Medium | UX | "Just Works" is fine for home audio; document the long-press pattern clearly; LED feedback on dongle. |
| Two dongles plugged into the same amp by mistake | Low | Confusion | Dongle reads amp serial number; advertises a name including those last 6 chars; user can tell them apart. |
| Future LE Audio-only speakers don't talk our service | Low | Integration scope | Out of scope for v1; v2 HID-over-GATT covers most foreign devices. |

## Open questions

1. **Encoder feel.** Detents (16/24 pulses per rev with click) vs
   continuous (smooth, no clicks). Strong subjective preference; both
   work. Default plan: detents, 24 PPR.
2. **Display orientation.** OLED facing up means knob lives flat;
   facing the user means it's a vertical puck. CAD calls this.
3. **Dongle reset button** — recessed pinhole vs no button (reset
   only via "hold encoder during knob's pairing-mode"). Latter is
   cleaner; former is more obvious.
4. **OTA firmware update for the dongle.** Wanted feature but adds
   ~30 KB and a pairing-mode key. Worth designing in from day one
   even if implemented in v1.5.
5. **Whether the dongle should also expose `Status_Flags` from day
   one.** Decoding the Hypex status-bits report is a future pcap
   session. v1 dongle can advertise the characteristic with all
   bits zero until decoded.

## Test plan — bringing the dongle up

Three tiers of test, each one a higher-fidelity check than the
previous. Each requires an additional bit of dongle firmware. Pick
where to stop based on how much end-to-end realism you need.

### Tier 1 — phone-app or web-bluetooth control (free, immediate)

Verifies the dongle's BLE peripheral side and the custom GATT
service. Use this for *every* dongle firmware iteration.

**Tooling:**
- **nRF Connect for Mobile** (Android / iOS, free) — scan, connect,
  read/write any characteristic by UUID. Use this for byte-level
  protocol bring-up.
- **Web Bluetooth in Chrome** — write a 50-line HTML page that
  exposes a slider and a mute button calling
  `characteristic.writeValue(...)`. Useful for showing the demo to
  a non-developer.

**What to verify:**
- Dongle advertises with name `FA503-XXXXXX` after booting and
  enumerating the amp.
- Connecting from the phone shows the custom service UUID.
- `volume_db_x100` write of `−3000` actually drops the speaker to
  −30 dB (audible / measurable on the amp).
- `mute.write(1)` mutes; `mute.write(0)` un-mutes; volume preserved
  across mute.
- Disconnecting and reconnecting works; bond persists.

This is the milestone that proves the architecture. Until tier 1
passes there is no point pursuing tiers 2 or 3.

### Tier 2 — off-the-shelf BLE volume knob (incremental control)

Verifies the *interop* path. The dongle adds HID-over-GATT host
support and pairs with a generic BLE remote that emits standard
Consumer Control HID keys (`Volume+`, `Volume−`, `Mute`,
`Play/Pause`).

**Mapping inside the dongle:**

```
HID Consumer Volume+      -> set volume = current + 50  (= +0.5 dB)
HID Consumer Volume-      -> set volume = current - 50
HID Consumer Mute         -> toggle mute
HID Consumer Play/Pause   -> toggle mute (alias)
```

Volume increment step is configurable on the dongle (NVS-stored,
default 0.5 dB).

**Recommended test hardware:**
- **Fosi Audio VOL20** (~$50) — established brand, BT + USB-C,
  decent feel. Caveat: product page doesn't confirm BLE; verify with
  nRF Connect before relying on it.
- **Adafruit ItsyBitsy nRF52840 + EC11 encoder** (~$25 total, half
  an hour of soldering) — guaranteed BLE HID via the Adafruit
  CircuitPython tutorial. Use as a backup or primary if BLE
  certainty matters.
- **Generic AliExpress BT volume knobs** (€10–25) — lottery on
  whether they're Classic or BLE; cheap enough to gamble.

**What to verify:**
- Dongle scans for, finds, and pairs with the BLE HID controller.
- A turn of the off-the-shelf knob produces a `Volume+` HID report
  on the dongle's BLE host stack.
- That report translates correctly to a Set State write on the USB
  side, with proper rate limiting (≥80 ms gap).
- The volume actually moves by the increment.
- Mute / unmute via the controller's button works.
- Two HID controllers paired simultaneously both work (validates
  multi-Central support — relevant for v2 knob co-existence).

### Tier 3 — bench-prototype "knob" (full custom service)

Validates the v1 knob firmware path before committing to v1
hardware. Build a minimal "fake knob" on a dev board.

**Hardware (~$10):**
- ESP32-C3 DevKitM-1 or nRF52840 dev board
- Bourns PEC11R rotary encoder
- A breadboard

**Firmware (~80 lines of ESP-IDF / Zephyr / Arduino):**
- BLE Central, scans for the dongle's service UUID
- On encoder rotate, write `volume_db_x100 = current ± step`
- On encoder press, toggle `mute`
- Subscribe to Notify on `volume_db_x100` and `mute` so the
  controller stays in sync if HFD (or another knob) changes state

**What to verify:**
- Sub-50 ms latency from encoder turn to volume change on the amp.
- No dropped writes during a fast turn (rate-limit handling working
  correctly inside the dongle, not at the controller).
- Notify subscription delivers state changes from concurrent writes
  on a different controller (e.g. the phone) within ~100 ms.

This *is* the v1 knob without the chassis. Once tier 3 passes, the
remaining work to ship the actual knob is hardware (display, encoder,
battery, enclosure) plus power-management firmware. The protocol
loop is proven.

### Test progression

```
   tier 1 (nRF Connect) ──► dongle GATT works
                                 │
                                 ▼
   tier 2 (off-the-shelf knob) ──► HID-host firmware works,
                                   foreign-device interop proven
                                 │
                                 ▼
   tier 3 (DIY breadboard knob) ──► v1 knob firmware path proven
                                 │
                                 ▼
   v1 knob hardware bring-up
```

Tier 1 is required. Tier 2 is "do this if you want the foreign-
device interop feature". Tier 3 is "do this before designing the
knob PCB so the firmware risk is retired first".

## Reference

- Hypex USB protocol: `knob/docs/HypexUsbProtocol.md`
- Experiment log: `knob/docs/experiments.md`
- BLE GATT spec: <https://www.bluetooth.com/specifications/specs/>
- nRF52840 SDK / Zephyr: <https://docs.zephyrproject.org/>
- ESP-IDF USB Host class: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_host.html>
- Volume Control Profile (VCP) — explicitly *not* used; for reference
  only: <https://www.bluetooth.com/specifications/specs/vcp-1-0/>
