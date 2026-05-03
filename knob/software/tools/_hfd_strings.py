"""Quick ASCII / UTF-16LE string extractor over HFD.exe with grep filter."""
import re
import sys
from pathlib import Path

p = Path(r"C:/Program Files (x86)/Hypex Software/Hypex filter design 5.2.4.24/HFD.exe")
data = p.read_bytes()

ascii_re = re.compile(rb"[\x20-\x7e]{5,}")
utf16le_re = re.compile(rb"(?:[\x20-\x7e]\x00){5,}")

ascii_strings = [m.group().decode("ascii", "replace") for m in ascii_re.finditer(data)]
utf16_strings = [m.group().decode("utf-16le", "replace") for m in utf16le_re.finditer(data)]

print(f"# {len(ascii_strings)} ASCII + {len(utf16_strings)} UTF-16LE strings")
all_strings = ascii_strings + utf16_strings

patterns = sys.argv[1:] or ["vu", "meter", "level", "peak", "rms",
                             "report", "hid", "endpoint", "ep_in",
                             "0x06", "0x05", "0x08", "GetInputReport",
                             "GetFeatureReport", "SetOutputReport",
                             "Read_PIC", "ReadPIC", "writeUSB", "readUSB"]

for pat in patterns:
    print(f"\n## matches for {pat!r}")
    rx = re.compile(re.escape(pat), re.IGNORECASE)
    seen = set()
    count = 0
    for s in all_strings:
        if rx.search(s) and s not in seen:
            seen.add(s)
            print(f"  {s[:160]}")
            count += 1
            if count >= 25:
                print(f"  ... (more)")
                break
