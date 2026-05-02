"""
Hypex FA503 USB HID protocol probe.

Implements the protocol from speakers/vendor/hypex/Hypex USB Hid documentation.pdf.

Install (Windows):
    pip install hidapi

Run:
    python hypex_probe.py list
    python hypex_probe.py status
    python hypex_probe.py set-volume -10.0
    python hypex_probe.py set-preset 2
    python hypex_probe.py mute
    python hypex_probe.py unmute
    python hypex_probe.py interactive
"""

import argparse
import sys

import hid

PACKET_LEN = 64

INPUT_NO_CHANGE = 0x00
INPUT_NAMES = {
    0x00: "SCAN/NoChange", 0x01: "XLR", 0x02: "RCA",
    0x04: "SPDIF", 0x05: "AES", 0x06: "OPT",
}


def find_hypex(vid=None, pid=None):
    matches = []
    for d in hid.enumerate(vid or 0, pid or 0):
        manuf = (d.get("manufacturer_string") or "").lower()
        prod = (d.get("product_string") or "").lower()
        if vid and pid:
            matches.append(d)
        elif "hypex" in manuf or "fusion" in prod or "fa50" in prod or "fa25" in prod:
            matches.append(d)
    return matches


def cmd_list(args):
    print("All HID devices:")
    for d in hid.enumerate():
        print(f"  VID=0x{d['vendor_id']:04x} PID=0x{d['product_id']:04x}  "
              f"{d.get('manufacturer_string')!r}  {d.get('product_string')!r}")
    print()
    matches = find_hypex(args.vid, args.pid)
    if matches:
        print(f"Likely Hypex matches ({len(matches)}):")
        for d in matches:
            print(f"  VID=0x{d['vendor_id']:04x} PID=0x{d['product_id']:04x}  "
                  f"{d.get('product_string')!r}  usage_page=0x{d['usage_page']:04x} "
                  f"usage=0x{d['usage']:04x}")
            print(f"    path={d['path']!r}")
    else:
        print("No Hypex-like devices found. Pass --vid/--pid explicitly,")
        print("or check the 'All HID devices' list above for the amp.")


def open_device(args):
    matches = find_hypex(args.vid, args.pid)
    if not matches:
        sys.exit("No matching device. Run `list` first.")
    # An HID device may expose multiple interfaces (collections) on Windows;
    # the control interface is usually the one whose usage_page is vendor-defined
    # (>= 0xff00). Prefer that if present.
    vendor = [m for m in matches if m["usage_page"] >= 0xff00]
    chosen = vendor[0] if vendor else matches[0]
    dev = hid.device()
    dev.open_path(chosen["path"])
    dev.set_nonblocking(False)
    return dev


def pad(data):
    # The Hypex HID interface has no Report ID, so hidapi.write() needs a
    # leading 0x00 (the "unnumbered report" marker, stripped before transmit)
    # followed by the 64-byte payload. Without it the first payload byte is
    # consumed as the report ID and the amp returns a zeroed packet.
    payload = list(data) + [0] * (PACKET_LEN - len(data))
    return [0x00] + payload[:PACKET_LEN]


def get_status(dev):
    dev.write(pad([0x06, 0x02, 0x00, 0x00]))
    return dev.read(PACKET_LEN, timeout_ms=1000)


def decode_status(resp):
    if not resp:
        return {"error": "no response"}
    return {
        "raw_first_8": " ".join(f"{b:02x}" for b in resp[:8]),
        "byte1_packet_id": f"0x{resp[1]:02x}" if len(resp) > 1 else None,
        "preset": resp[2] if len(resp) > 2 else None,
        "volume_db": (
            int.from_bytes(bytes(resp[3:5]), "little", signed=True) / 100.0
            if len(resp) > 4 else None
        ),
        "status_flags": f"0x{resp[6]:02x}" if len(resp) > 6 else None,
        "muted": bool(resp[6] & 0x80) if len(resp) > 6 else None,
    }


def set_state(dev, *, input_source=INPUT_NO_CHANGE, preset=None,
              volume_db=None, mute=None):
    # The Set State command is atomic — it carries ALL fields. For any field
    # the caller didn't specify, read the current value first so we don't
    # accidentally clobber it.
    need_current = preset is None or volume_db is None or mute is None
    current = get_status(dev) if need_current else None

    if preset is None:
        preset = current[2] if current else 1
    if volume_db is None:
        vol = (int.from_bytes(bytes(current[3:5]), "little", signed=True)
               if current else 0)
    else:
        vol = int(round(volume_db * 100))
    if mute is None:
        mute_byte = (current[6] & 0x80) if current else 0x00
    else:
        mute_byte = 0x80 if mute else 0x00

    vol &= 0xffff
    pkt = pad([0x05, input_source, preset, vol & 0xff, (vol >> 8) & 0xff,
               0x00, mute_byte])
    dev.write(pkt)
    return dev.read(PACKET_LEN, timeout_ms=1000)


def cmd_status(args):
    dev = open_device(args)
    try:
        resp = get_status(dev)
        print("Raw:", " ".join(f"{b:02x}" for b in resp))
        for k, v in decode_status(resp).items():
            print(f"  {k}: {v}")
    finally:
        dev.close()


def cmd_set_volume(args):
    dev = open_device(args)
    try:
        for k, v in decode_status(set_state(dev, volume_db=args.db)).items():
            print(f"  {k}: {v}")
    finally:
        dev.close()


def cmd_set_preset(args):
    dev = open_device(args)
    try:
        for k, v in decode_status(set_state(dev, preset=args.n)).items():
            print(f"  {k}: {v}")
    finally:
        dev.close()


def cmd_mute(args):
    dev = open_device(args)
    try:
        for k, v in decode_status(set_state(dev, mute=True)).items():
            print(f"  {k}: {v}")
    finally:
        dev.close()


def cmd_unmute(args):
    dev = open_device(args)
    try:
        for k, v in decode_status(set_state(dev, mute=False)).items():
            print(f"  {k}: {v}")
    finally:
        dev.close()


def cmd_interactive(args):
    dev = open_device(args)
    print("Interactive. Commands:  s | v <db> | p <1-3> | m | u | q")
    try:
        while True:
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            if not line:
                continue
            parts = line.split()
            c = parts[0]
            try:
                if c == "q":
                    break
                if c == "s":
                    r = get_status(dev)
                elif c == "v" and len(parts) == 2:
                    r = set_state(dev, volume_db=float(parts[1]))
                elif c == "p" and len(parts) == 2:
                    r = set_state(dev, preset=int(parts[1]))
                elif c == "m":
                    r = set_state(dev, mute=True)
                elif c == "u":
                    r = set_state(dev, mute=False)
                else:
                    print("?"); continue
                for k, v in decode_status(r).items():
                    print(f"  {k}: {v}")
            except Exception as e:
                print(f"error: {e}")
    finally:
        dev.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--vid", type=lambda x: int(x, 0),
                   help="USB vendor ID (e.g. 0x238b)")
    p.add_argument("--pid", type=lambda x: int(x, 0),
                   help="USB product ID")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("status")
    sv = sub.add_parser("set-volume"); sv.add_argument("db", type=float)
    sp = sub.add_parser("set-preset"); sp.add_argument("n", type=int, choices=[1, 2, 3])
    sub.add_parser("mute")
    sub.add_parser("unmute")
    sub.add_parser("interactive")

    args = p.parse_args()
    {
        "list": cmd_list,
        "status": cmd_status,
        "set-volume": cmd_set_volume,
        "set-preset": cmd_set_preset,
        "mute": cmd_mute,
        "unmute": cmd_unmute,
        "interactive": cmd_interactive,
    }[args.cmd](args)


if __name__ == "__main__":
    main()
