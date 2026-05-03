"""
Probe report-ID space for any responses that contain live data or new info.

For each (a, b) in the candidate space, send `a b 00 00 ...` and read the
response. Then for each report that returned a non-empty/non-trivial
response, do a back-to-back fluctuation test (3 captures) to see if any
bytes change while held.

Skips opcode 0x05 (Set State) and any opcode that returns the same first
byte as Set State response (0x05) — we don't want surprise writes.

Volume cap: -30 dB hard limit. State is not modified by this experiment.
"""
from __future__ import annotations

import json
import time
from datetime import datetime
from pathlib import Path

import hid

VID, PID = 0x345e, 0x03e8
PACKET_LEN = 64
RESULTS_DIR = (Path(__file__).resolve().parent.parent.parent
               / "docs" / "experiment_results")

# SAFETY: opcode 0x09 caused the FA503 to drop off USB and required a
# physical power cycle on 2026-05-02. Opcodes 0x07, 0x0a, 0x0b are
# untested. Only confirmed-read-only opcodes are enabled here. To re-add
# any first byte to this list, do it with the user present.
REPORT_A_VALUES = [0x03, 0x04, 0x06, 0x08]
REPORT_B_VALUES = list(range(0x00, 0x10))


def open_amp():
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev = hid.device()
            dev.open_path(d["path"])
            dev.set_nonblocking(False)
            return dev
    raise SystemExit("No amp found.")


def pad(payload):
    return [0x00] + list(payload) + [0] * (PACKET_LEN - len(payload))


def cmd(dev, payload):
    dev.write(pad(payload))
    return dev.read(PACKET_LEN, timeout_ms=300)


def drain(dev):
    dev.set_nonblocking(True)
    while True:
        if not dev.read(PACKET_LEN):
            break
    dev.set_nonblocking(False)


def is_trivial(resp):
    """All zero or echo of request."""
    if not resp:
        return True
    if all(b == 0 for b in resp):
        return True
    return False


def captures(dev, payload, n, gap_ms=50):
    out = []
    for _ in range(n):
        out.append(list(cmd(dev, payload)))
        time.sleep(gap_ms / 1000.0)
    return out


def main():
    dev = open_amp()
    drain(dev)

    print("# probing report-ID space (a, b) -> response")
    print(f"# a in {[hex(x) for x in REPORT_A_VALUES]}")
    print(f"# b in 0x00..0x0f")
    print()

    interesting = []
    for a in REPORT_A_VALUES:
        for b in REPORT_B_VALUES:
            payload = [a, b, 0x00, 0x00]
            resp = cmd(dev, payload)
            time.sleep(0.02)

            tag = f"{a:02x} {b:02x}"
            if is_trivial(resp):
                continue
            head = " ".join(f"{x:02x}" for x in resp[:32])
            print(f"  {tag} -> {head}{'...' if any(resp[32:]) else ''}")
            interesting.append((a, b, list(resp)))

    print(f"\n{len(interesting)} non-trivial responses out of "
          f"{len(REPORT_A_VALUES) * len(REPORT_B_VALUES)} probes\n")

    print("# fluctuation test on interesting reports (3x back-to-back)")
    fluct = {}
    for a, b, _ in interesting:
        caps = captures(dev, [a, b, 0x00, 0x00], 3, gap_ms=80)
        unique = {bytes(c).hex() for c in caps}
        live = []
        for i in range(PACKET_LEN):
            if len({c[i] for c in caps}) > 1:
                live.append(i)
        tag = f"{a:02x}_{b:02x}"
        fluct[tag] = {
            "unique_count": len(unique),
            "live_byte_positions": live,
            "captures": [bytes(c).hex() for c in caps],
        }
        marker = " *** LIVE ***" if live else ""
        print(f"  {a:02x} {b:02x}: unique={len(unique)} live_bytes={live}{marker}")

    out = RESULTS_DIR / f"probe_reports_{datetime.now().strftime('%Y%m%dT%H%M%S')}.json"
    out.write_text(json.dumps({
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "interesting_responses": [
            {"a": a, "b": b, "response_hex": bytes(r).hex()}
            for a, b, r in interesting
        ],
        "fluctuation": fluct,
    }, indent=2))
    print(f"\nresults -> {out}")
    dev.close()


if __name__ == "__main__":
    main()
