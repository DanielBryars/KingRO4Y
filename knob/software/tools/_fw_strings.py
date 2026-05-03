"""Decode Intel HEX firmware and extract strings."""
import re
import sys
from pathlib import Path

HEX = Path(r"C:/Program Files (x86)/Hypex Software/Hypex filter design 5.2.4.24/DSP3 firmware/DSP3-213 (FusionAmp)-v5.82.hex")

memory = {}
extbase = 0
for line in HEX.read_text().splitlines():
    if not line.startswith(":"):
        continue
    raw = bytes.fromhex(line[1:])
    bytecount = raw[0]
    addr = (raw[1] << 8) | raw[2]
    rectype = raw[3]
    data = raw[4:4 + bytecount]
    if rectype == 0x00:
        flat = (extbase << 16) | addr
        for i, b in enumerate(data):
            memory[flat + i] = b
    elif rectype == 0x04:
        extbase = (data[0] << 8) | data[1]
    elif rectype == 0x01:
        break
    elif rectype == 0x05:
        pass

if not memory:
    raise SystemExit("no data extracted")

addrs = sorted(memory.keys())
lo, hi = addrs[0], addrs[-1]
print(f"# firmware: {len(memory)} bytes mapped, range 0x{lo:08x}..0x{hi:08x}")

flat = bytearray(hi - lo + 1)
for a, b in memory.items():
    flat[a - lo] = b

ascii_re = re.compile(rb"[\x20-\x7e]{6,}")
strings_with_addr = []
for m in ascii_re.finditer(flat):
    strings_with_addr.append((lo + m.start(), m.group().decode("ascii", "replace")))

print(f"# {len(strings_with_addr)} ASCII strings (>=6 chars)")

patterns = sys.argv[1:] or ["meter", "peak", "vu", "report", "command",
                             "DSP3", "FUSION", "version", "status", "signal"]

for pat in patterns:
    print(f"\n## {pat!r}")
    rx = re.compile(re.escape(pat), re.IGNORECASE)
    seen = set()
    count = 0
    for addr, s in strings_with_addr:
        if rx.search(s) and s not in seen:
            seen.add(s)
            print(f"  0x{addr:08x}  {s[:160]}")
            count += 1
            if count >= 30:
                print("  ... more")
                break
