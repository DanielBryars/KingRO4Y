"""
Experiment: decode Get Report 06 03 (sensor/runtime block).

Strategy:
  1. Capture 10 back-to-back responses at the current state — if bytes
     fluctuate between captures, those positions are live (meters); if
     identical, they're static config.
  2. Hold state at preset 1 / -50 dB / mute ON for 1 s, capture 5 times
     (no audio output).
  3. Hold state at preset 1 / -50 dB / mute OFF for 1 s, capture 5 times
     (output, low level).
  4. Hold state at preset 1 / -30 dB / mute OFF for 1 s, capture 5 times
     (output, max permitted level).
  5. Restore baseline.

Diff is computed across all (1) captures vs (2)/(3)/(4) groups so live
bytes and signal-correlated bytes can be told apart.

Volume cap: -30 dB hard limit.
"""
from __future__ import annotations

import json
import time
from datetime import datetime
from pathlib import Path

import hid

VID, PID = 0x345e, 0x03e8
PACKET_LEN = 64
VOL_CAP_CENTIDB = -3000
RESULTS_DIR = Path(__file__).resolve().parent.parent.parent / "docs" / "experiment_results"


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
    return dev.read(PACKET_LEN, timeout_ms=500)


def drain(dev):
    dev.set_nonblocking(True)
    while True:
        if not dev.read(PACKET_LEN):
            break
    dev.set_nonblocking(False)


def status(dev):
    return cmd(dev, [0x06, 0x02, 0x00, 0x00])


def report_06_03(dev):
    return cmd(dev, [0x06, 0x03, 0x00, 0x00])


def safe_set_state(dev, *, input_source, preset, vol_centidb, mute):
    if vol_centidb > VOL_CAP_CENTIDB:
        raise SystemExit(f"safety: vol {vol_centidb} > cap {VOL_CAP_CENTIDB}")
    v = vol_centidb & 0xffff
    return cmd(dev, [0x05, input_source, preset,
                     v & 0xff, (v >> 8) & 0xff,
                     0x00, 0x80 if mute else 0x00])


def captures(dev, fn, n, gap_ms=80):
    out = []
    for _ in range(n):
        out.append(list(fn(dev)))
        time.sleep(gap_ms / 1000.0)
    return out


def diff_positions(groups):
    """Return list of byte positions whose value varies anywhere across groups."""
    positions = []
    for i in range(PACKET_LEN):
        seen = set()
        for g in groups:
            for cap in g:
                seen.add(cap[i])
        if len(seen) > 1:
            positions.append(i)
    return positions


def hexrow(cap, positions):
    return [f"{cap[i]:02x}" for i in positions]


def main():
    dev = open_amp()
    drain(dev)

    base = status(dev)
    orig_preset = base[2]
    orig_vol = int.from_bytes(bytes(base[3:5]), "little", signed=True)
    orig_mute = bool(base[6] & 0x80)
    print(f"baseline: preset={orig_preset}, vol={orig_vol/100} dB, "
          f"mute={orig_mute}")

    if orig_vol > VOL_CAP_CENTIDB:
        print(f"baseline volume {orig_vol/100} dB above cap; clamping for safety")
        safe_set_state(dev, input_source=0x00, preset=orig_preset,
                       vol_centidb=VOL_CAP_CENTIDB, mute=orig_mute)
        drain(dev)
        orig_vol = VOL_CAP_CENTIDB

    results = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "baseline": {"preset": orig_preset, "vol_centidb": orig_vol,
                     "mute": orig_mute},
        "groups": {},
    }

    # 1. Live fluctuation at baseline
    print("\n[1/4] fluctuation test at baseline (10x)")
    safe_set_state(dev, input_source=0x00, preset=orig_preset,
                   vol_centidb=orig_vol, mute=orig_mute)
    drain(dev)
    g_baseline = captures(dev, report_06_03, 10)
    results["groups"]["baseline_fluct"] = [bytes(c).hex() for c in g_baseline]

    # 2. Mute ON, -50 dB
    print("[2/4] mute ON @ -50 dB (5x)")
    safe_set_state(dev, input_source=0x00, preset=orig_preset,
                   vol_centidb=-5000, mute=True)
    time.sleep(1.0)
    drain(dev)
    g_mute = captures(dev, report_06_03, 5)
    results["groups"]["mute_on_-50dB"] = [bytes(c).hex() for c in g_mute]

    # 3. Mute OFF, -50 dB (low signal)
    print("[3/4] mute OFF @ -50 dB (5x)")
    safe_set_state(dev, input_source=0x00, preset=orig_preset,
                   vol_centidb=-5000, mute=False)
    time.sleep(1.0)
    drain(dev)
    g_low = captures(dev, report_06_03, 5)
    results["groups"]["live_-50dB"] = [bytes(c).hex() for c in g_low]

    # 4. Mute OFF, -30 dB (capped peak)
    print("[4/4] mute OFF @ -30 dB (5x)")
    safe_set_state(dev, input_source=0x00, preset=orig_preset,
                   vol_centidb=-3000, mute=False)
    time.sleep(1.0)
    drain(dev)
    g_high = captures(dev, report_06_03, 5)
    results["groups"]["live_-30dB"] = [bytes(c).hex() for c in g_high]

    # Restore baseline
    print("\nrestoring baseline")
    safe_set_state(dev, input_source=0x00, preset=orig_preset,
                   vol_centidb=orig_vol, mute=orig_mute)
    drain(dev)

    # Analysis
    all_groups = [g_baseline, g_mute, g_low, g_high]
    changing = diff_positions(all_groups)
    print(f"\nbytes that vary: {len(changing)} positions")

    print("\n  pos | base[0,5,9] | muteON[0,4] | low[0,4] | hi[0,4]")
    print("  ----|-------------|-------------|----------|--------")
    for i in changing:
        cells = [
            f"{g_baseline[0][i]:02x}/{g_baseline[5][i]:02x}/{g_baseline[9][i]:02x}",
            f"{g_mute[0][i]:02x}/{g_mute[4][i]:02x}",
            f"{g_low[0][i]:02x}/{g_low[4][i]:02x}",
            f"{g_high[0][i]:02x}/{g_high[4][i]:02x}",
        ]
        print(f"  {i:>3} | {cells[0]:>11} | {cells[1]:>11} | {cells[2]:>8} | {cells[3]:>6}")

    # Live-only positions: vary within group_baseline alone
    base_only_changing = []
    for i in range(PACKET_LEN):
        vals = {c[i] for c in g_baseline}
        if len(vals) > 1:
            base_only_changing.append(i)
    print(f"\nbytes fluctuating WITHIN baseline group (live meters): "
          f"{base_only_changing}")

    results["analysis"] = {
        "changing_positions": changing,
        "live_within_baseline": base_only_changing,
    }
    out = RESULTS_DIR / f"exp_06_03_{datetime.now().strftime('%Y%m%dT%H%M%S')}.json"
    out.write_text(json.dumps(results, indent=2))
    print(f"\nfull results -> {out}")

    dev.close()


if __name__ == "__main__":
    main()
