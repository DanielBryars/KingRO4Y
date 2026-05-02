# Hypex Fusion Control Reverse Engineering Notes

## Context

This document captures findings from:

- DIYAudio thread on Hypex Fusion remote/display
- Reverse-engineered USB HID protocol
- GitHub project: UsbAmpControl
- Practical system considerations for FA503-based builds

---

## 1. Key Insight: State-Based Control Model

The Hypex Fusion control system is **state-based**, not event-based.

Each command defines a *complete amplifier state*:

- Input source
- Preset
- Volume
- Mute

---

## 2. USB HID Protocol Summary

### Transport

- USB HID (Class 0x03)
- Fixed 64-byte packets
- OUT endpoint: `0x01`
- IN endpoint: `0x81`
- Strict command → response pattern

---

### Command Packet (Host → Amp)

| Byte | Field         | Description |
|------|--------------|-------------|
| 0    | Command ID    | `0x05` = Set State |
| 1    | Input Source  | Enum |
| 2    | Preset        | 1–3 |
| 3–4  | Volume        | int16 LE, dB × 100 |
| 6    | Mute flag     | Bit 7 |

---

### Volume Encoding

value = dB * 100

Examples:

| dB     | Value | Hex (LE) |
|--------|------|----------|
| -3.0   | -300 | D4 FE |
| -10.0  | -1000| 18 FC |
| 0.0    | 0    | 00 00 |

---

### Status Packet (Amp → Host)

| Byte | Field           |
|------|----------------|
| 1    | Packet ID       |
| 2    | Current preset  |
| 3–4  | Current volume  |
| 6    | Status flags    |

---

## 3. GitHub Confirmation

Repo: UsbAmpControl

Confirms working HID control implementation with ESP32-S3.

---

## 4. OLED / Remote Interface Insights

- Remote kit = IR + LED only
- OLED display uses a UART-like interface
- Display cycles preset / input / volume

---

## 5. Likely Internal Architecture

IR / USB / Display → Control MCU → DSP → Audio

---

## 6. UART Hypothesis

Likely same fields:

- preset (uint8)
- volume (int16 LE, dB × 100)
- mute (bit flag)
- input (enum)

Different transport vs USB.

---

## 7. Reverse Engineering Strategy

Test:

- Volume → look for int16 LE patterns
- Preset → 01 / 02 / 03
- Mute → bit flip (likely 0x80)
- Input → 00 / 01 / 02 / 04 / 05 / 06

---

## 8. System Implications

- Volume is applied in DSP
- Works for analogue and digital inputs
- Multi-amp sync handled internally

---

## 9. Design Options

A. IR → simple  
B. USB HID → recommended  
C. UART → advanced  
D. Analogue preamp → not ideal  

---

## 10. Recommendation

Start with USB HID, then explore UART if needed.

---

## Key Insight

Volume encoding:

int16, little-endian, dB × 100
