"""Focus on the 248-count polling loop (`05 01 01 d8 ...` OUT) and decode
which bytes in the IN response are the VU meter."""
import sys
from pathlib import Path

TSV = Path(r"E:/git/KingRO4Y/knob/docs/experiment_results/hfd_traffic.tsv")

sys.stdout.reconfigure(encoding="utf-8")


def to_bytes(hexstr):
    return [int(hexstr[i:i+2], 16) for i in range(0, len(hexstr), 2)]


def main():
    rows = []
    with TSV.open() as f:
        next(f)
        for line in f:
            frame, t, d, data = line.rstrip().split("\t")
            rows.append({"frame": int(frame), "t": float(t),
                         "dir": d, "data": data, "bytes": to_bytes(data)})

    # Identify the polling pairs: OUT starts with 05 01 01 d8.
    polls = []
    for i, r in enumerate(rows):
        if r["dir"] == "OUT" and r["data"].startswith("050101d8") and i + 1 < len(rows):
            nxt = rows[i + 1]
            if nxt["dir"] == "IN ":
                polls.append((r, nxt))

    print(f"# polling pairs found: {len(polls)}")
    if not polls:
        return

    # Show byte variance across the 248 IN responses.
    n = 64
    varying = []
    for b in range(n):
        vals = {p[1]["bytes"][b] for p in polls}
        if len(vals) > 1:
            varying.append((b, sorted(vals)))

    print(f"\n# IN response bytes that vary across {len(polls)} polls")
    for b, vals in varying:
        sample = vals[:8]
        print(f"  byte {b:>2}: {len(vals):>3} unique  range {min(vals):>3}..{max(vals):>3}  e.g. {sample}")

    # First / middle / last poll detail
    print("\n# representative polls (first, middle, last)")
    for label, p in [("first", polls[0]),
                     ("mid",   polls[len(polls)//2]),
                     ("last",  polls[-1])]:
        bs = p[1]["bytes"]
        chunks = [" ".join(f"{b:02x}" for b in bs[i:i+16]) for i in range(0, 64, 16)]
        print(f"  {label} (f{p[0]['frame']} t={p[0]['t']:.2f}s):")
        for c in chunks:
            print(f"    {c}")

    # Identify quiet periods: gaps >300 ms between consecutive polls.
    print("\n# quiet periods (>300 ms gap) — likely VU meter on/off boundaries")
    gaps = []
    for i in range(len(polls) - 1):
        dt = polls[i+1][0]["t"] - polls[i][0]["t"]
        if dt > 0.3:
            gaps.append((polls[i][0]["t"], polls[i+1][0]["t"], dt))
    for start, end, dt in gaps:
        print(f"  silence: {dt*1000:.0f} ms  from t={start:.2f}s to t={end:.2f}s")

    # Which bytes track audio levels? Look at a busy stretch — find values
    # at known-meter byte indices and print a time series.
    print("\n# time-series of suspected meter bytes (47, 48, 49) over first 30 polls")
    print("  frame    t   b46 b47 b48 b49 b50")
    for p in polls[:30]:
        bs = p[1]["bytes"]
        print(f"  f{p[0]['frame']:>5} {p[0]['t']:>6.2f}  "
              f"{bs[46]:>3} {bs[47]:>3} {bs[48]:>3} {bs[49]:>3} {bs[50]:>3}")

    # Find consecutive pairs of bytes that look like 16-bit LE numbers
    # tracking peak — print the two strongest candidates.
    print("\n# candidate 16-bit LE meter values across all polls")
    candidates = {}
    for offset in range(63):
        vals = [(p[1]["bytes"][offset] | (p[1]["bytes"][offset+1] << 8))
                for p in polls]
        unique = len(set(vals))
        if unique < 5:
            continue
        candidates[offset] = (unique, min(vals), max(vals), vals[:6])
    for offset, (uniq, lo, hi, sample) in sorted(candidates.items(), key=lambda x: -x[1][0])[:10]:
        print(f"  bytes {offset}-{offset+1}: {uniq} unique LE16 values, "
              f"range {lo}..{hi}, e.g. {sample}")


if __name__ == "__main__":
    main()
