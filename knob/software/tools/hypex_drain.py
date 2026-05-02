"""Drain any queued IN packets, then read fresh status."""
import hid

VID, PID = 0x345e, 0x03e8


def main():
    dev = hid.device()
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev.open_path(d["path"])
            break
    dev.set_nonblocking(True)
    drained = 0
    while True:
        r = dev.read(64)
        if not r:
            break
        drained += 1
        print(f"  drained: {' '.join(f'{b:02x}' for b in r[:16])}...")
    print(f"drained {drained} packets")

    dev.set_nonblocking(False)
    print("\nfresh get-status:")
    dev.write([0x00, 0x06, 0x02, 0x00, 0x00] + [0] * 59)
    r = dev.read(64, timeout_ms=1000)
    print(" ".join(f"{b:02x}" for b in r))
    if r and len(r) > 6:
        vol = int.from_bytes(bytes(r[3:5]), "little", signed=True) / 100.0
        print(f"  preset={r[2]}  volume={vol} dB  muted={bool(r[6] & 0x80)}  "
              f"packet_id=0x{r[1]:02x}")
    dev.close()


if __name__ == "__main__":
    main()
