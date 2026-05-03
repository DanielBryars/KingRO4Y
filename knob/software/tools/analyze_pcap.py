"""
Parse UsbCapture.pcapng via tshark, isolate FA503 (device address 18)
interrupt OUT (0x01) and IN (0x81) traffic, and analyze the protocol
HFD uses.
"""
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

PCAP = Path(r"E:/git/KingRO4Y/UsbCapture.pcapng")
TSHARK = Path(r"C:/Program Files/Wireshark/tshark.exe")
DEV_ADDR = "18"


def run_tshark():
    # The vendor HID payload appears in `usbhid.data`, not `usb.capdata`,
    # because Wireshark's HID dissector handles this device.
    cmd = [
        str(TSHARK), "-r", str(PCAP), "-T", "fields",
        "-e", "frame.number",
        "-e", "frame.time_relative",
        "-e", "usb.device_address",
        "-e", "usb.endpoint_address",
        "-e", "usbhid.data",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.stdout


def parse(raw):
    rows = []
    for line in raw.splitlines():
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        frame, t, dev, ep, data = parts[:5]
        if dev != DEV_ADDR:
            continue
        if ep not in ("0x01", "0x81"):
            continue
        if not data:
            continue
        rows.append({
            "frame": int(frame),
            "t": float(t),
            "dir": "OUT" if ep == "0x01" else "IN ",
            "data": data,
        })
    return rows


def main():
    print("running tshark...", file=sys.stderr)
    raw = run_tshark()
    rows = parse(raw)
    print(f"got {len(rows)} interrupt OUT/IN packets with payload\n", file=sys.stderr)

    # First 20 lines of dialog
    print("# first 30 OUT/IN exchanges")
    for r in rows[:30]:
        head = r["data"][:32]
        print(f"  f{r['frame']:>5} {r['t']:>8.3f}s {r['dir']} {head}…")

    # Count unique OUT request "headers" (first 4 bytes)
    out_headers = Counter()
    out_first_byte = Counter()
    in_first_byte = Counter()
    for r in rows:
        head4 = r["data"][:8]   # 4 bytes hex
        if r["dir"] == "OUT":
            out_headers[head4] += 1
            out_first_byte[r["data"][:2]] += 1
        else:
            in_first_byte[r["data"][:2]] += 1

    print(f"\n# OUT requests by first 4 bytes (top 30)")
    for h, n in out_headers.most_common(30):
        print(f"  {n:>5}× {h}")

    print(f"\n# OUT first byte distribution")
    for b, n in sorted(out_first_byte.items()):
        print(f"  {n:>5}× 0x{b}")

    print(f"\n# IN  first byte distribution")
    for b, n in sorted(in_first_byte.items()):
        print(f"  {n:>5}× 0x{b}")

    # Save all rows to CSV-like for later
    out = Path(r"E:/git/KingRO4Y/knob/docs/experiment_results/hfd_traffic.tsv")
    with out.open("w", encoding="utf8") as f:
        f.write("frame\ttime_s\tdir\tdata\n")
        for r in rows:
            f.write(f"{r['frame']}\t{r['t']:.6f}\t{r['dir']}\t{r['data']}\n")
    print(f"\nsaved full traffic to {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
