"""
Protocol exploration script. Captures responses for:
  - Filter name read (03 08 00 00)
  - Get Reports 06 01 and 06 03 (PDF mentions, content unknown)
  - Full 64-byte status response across volume / mute / preset changes

All writes round-trip current state. Baseline is captured first and restored
at the end. Output is markdown for pasting into knob/docs/experiments.md.
"""
import time

import hid

VID, PID = 0x345e, 0x03e8
PACKET_LEN = 64
SAFE_VOL_CENTIDB = -5000  # -50 dB while we sweep state


def open_amp():
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev = hid.device()
            dev.open_path(d["path"])
            dev.set_nonblocking(False)
            return dev
    raise SystemExit("No Hypex amp found.")


def pad(payload):
    return [0x00] + list(payload) + [0] * (PACKET_LEN - len(payload))


def cmd(dev, payload):
    dev.write(pad(payload))
    return dev.read(PACKET_LEN, timeout_ms=500)


def drain(dev):
    dev.set_nonblocking(True)
    n = 0
    while True:
        r = dev.read(PACKET_LEN)
        if not r:
            break
        n += 1
    dev.set_nonblocking(False)
    return n


def status(dev):
    return cmd(dev, [0x06, 0x02, 0x00, 0x00])


def set_state(dev, *, input_source, preset, vol_centidb, mute):
    v = vol_centidb & 0xffff
    return cmd(dev, [0x05, input_source, preset, v & 0xff, (v >> 8) & 0xff,
                     0x00, 0x80 if mute else 0x00])


def hexline(b, limit=None):
    if not b:
        return "<empty>"
    bs = b[:limit] if limit else b
    s = " ".join(f"{x:02x}" for x in bs)
    if limit and len(b) > limit:
        s += " ..."
    return s


def ascii_run(b, start):
    """Return the longest ASCII run starting at offset `start` (printable only)."""
    out = []
    for x in b[start:]:
        if 32 <= x < 127:
            out.append(chr(x))
        else:
            break
    return "".join(out)


def parse_status_basics(r):
    return {
        "preset": r[2],
        "vol_db": int.from_bytes(bytes(r[3:5]), "little", signed=True) / 100.0,
        "mute": bool(r[6] & 0x80),
        "byte50": r[50] if len(r) > 50 else None,
        "byte56_57": (r[56], r[57]) if len(r) > 57 else None,
    }


def main():
    dev = open_amp()

    n = drain(dev)
    if n:
        print(f"(drained {n} stale packets)\n")

    print("# Protocol exploration — 2026-05-02\n")

    print("## Baseline (read on entry)\n")
    base = status(dev)
    bp = parse_status_basics(base)
    print(f"- preset={bp['preset']}, volume={bp['vol_db']} dB, mute={bp['mute']}")
    print(f"- byte50=0x{bp['byte50']:02x}, byte56-57={bp['byte56_57']}")
    print(f"- raw: `{hexline(base)}`\n")

    orig_input = 0x00  # never overwrite, use 0x00 = no change
    orig_preset = base[2]
    orig_vol = int.from_bytes(bytes(base[3:5]), "little", signed=True)
    orig_mute = bool(base[6] & 0x80)

    # --- read-only commands ---
    print("## Read-only commands\n")

    print("### Filter name (03 08 00 00)")
    r = cmd(dev, [0x03, 0x08, 0x00, 0x00])
    print(f"- raw: `{hexline(r)}`")
    for offset in (0, 1, 2, 3, 4):
        s = ascii_run(r, offset)
        if len(s) >= 4:
            print(f"- ASCII run at byte {offset}: `{s!r}`")
    print()

    print("### Get Report 06 01")
    r = cmd(dev, [0x06, 0x01, 0x00, 0x00])
    print(f"- raw: `{hexline(r)}`")
    print()

    print("### Get Report 06 03")
    r = cmd(dev, [0x06, 0x03, 0x00, 0x00])
    print(f"- raw: `{hexline(r)}`")
    print()

    # --- step volume down to a quieter level for state-change captures ---
    print("## Lowering volume to -50 dB before state-change captures\n")
    set_state(dev, input_source=0x00, preset=orig_preset,
              vol_centidb=SAFE_VOL_CENTIDB, mute=orig_mute)
    drain(dev)

    captures = []

    def capture(label, *, preset, vol_centidb, mute):
        set_state(dev, input_source=0x00, preset=preset,
                  vol_centidb=vol_centidb, mute=mute)
        time.sleep(0.05)
        drain(dev)
        s = status(dev)
        captures.append((label, list(s)))
        ps = parse_status_basics(s)
        print(f"- **{label}**: preset={ps['preset']}, vol={ps['vol_db']} dB, "
              f"mute={ps['mute']}, byte56_57={ps['byte56_57']}")
        print(f"  - raw: `{hexline(s)}`")

    print("## Status-tail captures\n")

    print("### Volume sweep, preset {}, mute off".format(orig_preset))
    capture("v=-60 dB", preset=orig_preset, vol_centidb=-6000, mute=False)
    capture("v=-50 dB", preset=orig_preset, vol_centidb=-5000, mute=False)
    capture("v=-40 dB", preset=orig_preset, vol_centidb=-4000, mute=False)
    print()

    print("### Mute toggle, preset {}, vol -50 dB".format(orig_preset))
    capture("mute=ON",  preset=orig_preset, vol_centidb=-5000, mute=True)
    capture("mute=OFF", preset=orig_preset, vol_centidb=-5000, mute=False)
    print()

    print("### Preset switch, vol -50 dB, mute off")
    capture("preset 1", preset=1, vol_centidb=-5000, mute=False)
    capture("preset 2", preset=2, vol_centidb=-5000, mute=False)
    capture("preset 3", preset=3, vol_centidb=-5000, mute=False)
    print()

    # --- diff ---
    print("## Byte-position diff (only positions that change)\n")
    labels = [c[0] for c in captures]
    print("| byte | " + " | ".join(labels) + " |")
    print("|------|" + "|".join(["------"] * len(labels)) + "|")
    for i in range(PACKET_LEN):
        col = [c[1][i] for c in captures]
        if any(v != col[0] for v in col):
            row = " | ".join(f"`{v:02x}`" for v in col)
            print(f"| {i:>2} | {row} |")
    print()

    # --- restore ---
    print("## Restore baseline\n")
    set_state(dev, input_source=0x00, preset=orig_preset,
              vol_centidb=orig_vol, mute=orig_mute)
    drain(dev)
    s = status(dev)
    sp = parse_status_basics(s)
    print(f"- Restored: preset={sp['preset']}, vol={sp['vol_db']} dB, "
          f"mute={sp['mute']}")

    dev.close()


if __name__ == "__main__":
    main()
