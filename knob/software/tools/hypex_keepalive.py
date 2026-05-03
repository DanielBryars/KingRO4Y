"""
Wait for the FA503 to re-enumerate, then send a get-status every 5 s so
the amp does not drop back into standby while HFD is being used to
re-upload the project.

Read-only — no Set State writes.
Press Ctrl+C to stop.
"""
import time
from datetime import datetime

import hid

VID, PID = 0x345e, 0x03e8


def find_amp():
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            return d
    return None


def main():
    print("waiting for amp to enumerate (Ctrl+C to stop)...")
    while find_amp() is None:
        time.sleep(2)

    info = find_amp()
    print(f"amp present. serial={info.get('serial_number_string')!r}")

    dev = hid.device()
    dev.open_path(info["path"])
    dev.set_nonblocking(False)

    print("sending get-status every 5 s. open HFD now and upload the "
          "project. Ctrl+C when done.\n")

    while True:
        try:
            dev.write([0x00, 0x06, 0x02, 0x00, 0x00] + [0] * 60)
            r = dev.read(64, timeout_ms=500)
            ts = datetime.now().strftime("%H:%M:%S")
            if r and len(r) > 6:
                vol = int.from_bytes(bytes(r[3:5]), "little", signed=True) / 100.0
                muted = bool(r[6] & 0x80)
                print(f"  {ts}  preset={r[2]}  vol={vol} dB  mute={muted}  "
                      f"input(b50)=0x{r[50]:02x}")
            else:
                print(f"  {ts}  empty/short response — amp may have dropped")
        except OSError as e:
            print(f"  amp error: {e}; will reconnect")
            try:
                dev.close()
            except Exception:
                pass
            while find_amp() is None:
                time.sleep(2)
            info = find_amp()
            dev = hid.device()
            dev.open_path(info["path"])
            dev.set_nonblocking(False)
            print(f"  reconnected. serial={info.get('serial_number_string')!r}")
        time.sleep(5)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped.")
