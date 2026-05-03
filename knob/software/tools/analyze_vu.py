"""Drill into the 0x05 polling loop and the 0x65/0x66/0x67 family to see
which exchange is the VU meter, and decode its bytes."""
from collections import Counter
from pathlib import Path

TSV = Path(r"E:/git/KingRO4Y/knob/docs/experiment_results/hfd_traffic.tsv")


def main():
    rows = []
    with TSV.open() as f:
        next(f)
        for line in f:
            frame, t, d, data = line.rstrip().split("\t")
            rows.append({"frame": int(frame), "t": float(t), "dir": d, "data": data})

    # Show distinct OUT shapes and their counts.
    out_shapes = Counter()
    for r in rows:
        if r["dir"] == "OUT":
            out_shapes[r["data"][:8]] += 1
    print("# OUT first-4-byte shapes (top 20)")
    for shape, n in out_shapes.most_common(20):
        bytes_str = " ".join(shape[i:i+2] for i in range(0, 8, 2))
        print(f"  {n:>5}× {bytes_str}")

    # The 0x05 polling loop — show timestamp gaps and unique data shapes.
    print("\n# 0x05 OUT timing")
    times = [r["t"] for r in rows if r["dir"] == "OUT" and r["data"].startswith("05")]
    if len(times) >= 2:
        gaps = [times[i+1] - times[i] for i in range(len(times) - 1)]
        print(f"  count: {len(times)}")
        print(f"  mean gap: {sum(gaps)/len(gaps)*1000:.1f} ms")
        print(f"  min gap: {min(gaps)*1000:.1f} ms")
        print(f"  max gap: {max(gaps)*1000:.1f} ms")
        # Histogram of gaps
        bins = [0.005, 0.01, 0.02, 0.05, 0.1, 0.5, 1.0, 5.0]
        hist = [0] * (len(bins) + 1)
        for g in gaps:
            for i, b in enumerate(bins):
                if g < b:
                    hist[i] += 1
                    break
            else:
                hist[-1] += 1
        labels = [f"<{b*1000:.0f}ms" for b in bins] + [f">={bins[-1]*1000:.0f}ms"]
        for lab, n in zip(labels, hist):
            if n:
                print(f"    {lab:<10}: {n}")

    # Look at first 0x65/0x66/0x67 exchanges — show full 64 bytes
    print("\n# Non-0x03/05/06 exchanges (full 64 bytes)")
    interesting_first_bytes = ("65", "66", "67")
    for r in rows:
        if r["data"][:2] in interesting_first_bytes:
            d = r["data"]
            print(f"  f{r['frame']:>5} {r['t']:>8.3f}s {r['dir']} "
                  f"{' '.join(d[i:i+2] for i in range(0, len(d), 2))}")

    # Show 5 consecutive 0x05 OUT/IN pairs in detail
    print("\n# 0x05 OUT->IN pairs (first 6, full 64 bytes)")
    seen = 0
    for i, r in enumerate(rows):
        if r["dir"] == "OUT" and r["data"].startswith("05") and i + 1 < len(rows):
            nxt = rows[i + 1]
            d_o = " ".join(r["data"][j:j+2] for j in range(0, len(r["data"]), 2))
            d_i = " ".join(nxt["data"][j:j+2] for j in range(0, len(nxt["data"]), 2))
            print(f"  f{r['frame']} OUT  {d_o}")
            print(f"  f{nxt['frame']} IN   {d_i}")
            seen += 1
            if seen >= 6:
                break

    # Detect changes in the 0x05 IN responses (which bytes vary?)
    print("\n# 0x05 IN response — bytes that vary across the polling loop")
    in_responses = [r["data"] for r in rows if r["dir"] == "IN" and r["data"].startswith("05")]
    if in_responses:
        n_bytes = len(in_responses[0]) // 2
        varying = []
        for b in range(n_bytes):
            vals = {resp[b*2:b*2+2] for resp in in_responses}
            if len(vals) > 1:
                varying.append((b, vals))
        print(f"  total 0x05 IN responses: {len(in_responses)}")
        print(f"  bytes that vary across them: {len(varying)}")
        for byte_idx, vals in varying[:30]:
            samples = sorted(vals)[:6]
            print(f"    byte {byte_idx:>2}: {len(vals)} unique values, e.g. {samples}")


if __name__ == "__main__":
    main()
