"""One-shot: restore amp to a safe state (preset 1, -60 dB, muted)."""
import hid

VID, PID = 0x345e, 0x03e8


def main():
    dev = hid.device()
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev.open_path(d["path"])
            break
    dev.set_nonblocking(False)

    vol = -6000  # -60.00 dB
    cmd = [0x00,                    # hidapi: "no report ID"
           0x05,                    # Set State
           0x00,                    # input source: no change
           0x01,                    # preset 1
           vol & 0xff, (vol >> 8) & 0xff,
           0x00,
           0x80,                    # mute ON
           ]
    cmd += [0] * (65 - len(cmd))
    dev.write(cmd)
    r = dev.read(64, timeout_ms=1000)
    print("response:", " ".join(f"{b:02x}" for b in r[:8]), "...")

    dev.write([0x00, 0x06, 0x02, 0x00, 0x00] + [0] * 60)
    r = dev.read(64, timeout_ms=1000)
    print("status:  ", " ".join(f"{b:02x}" for b in r[:8]), "...")
    if r:
        v = int.from_bytes(bytes(r[3:5]), "little", signed=True) / 100.0
        print(f"  preset={r[2]}  volume={v} dB  muted={bool(r[6] & 0x80)}")
    dev.close()


if __name__ == "__main__":
    main()
