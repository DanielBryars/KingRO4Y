"""
Live VU meter for the Hypex FA503.

Mirrors HFD's VU polling loop verbatim — opcode 0x05 with byte 1 = 0x01
("give me state plus current peak meter"). Polls at ~10 Hz and prints a
horizontal bar of the meter value plus a peak-hold marker.

The polling packet is built by reading the amp's current status, copying
that response, flipping byte 1 to 0x01 to mark it as a meter request,
and zeroing the response-only fields. This means we always send a
project-signature-correct packet without hardcoding values.

Exit with Ctrl+C.

Verified safe — uses only opcodes 0x06 0x02 (read) and 0x05 0x01
(documented polling, not Set State).
"""
import math
import sys
import time

import hid

VID, PID = 0x345e, 0x03e8
PACKET_LEN = 64
POLL_HZ = 10
PEAK_HOLD_S = 1.5

# Heuristic for converting raw LE16 (bytes 47-48) to dB FS. Treats the
# value as linear amplitude on [0, 32767] -> [-inf, 0] dB FS.
DB_FLOOR = -60.0


def open_amp():
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev = hid.device()
            dev.open_path(d["path"])
            dev.set_nonblocking(False)
            return dev
    raise SystemExit("No FA503 found.")


def hidwrite(dev, payload):
    dev.write([0x00] + list(payload) + [0] * (PACKET_LEN - len(payload)))


def get_status(dev):
    hidwrite(dev, [0x06, 0x02, 0x00, 0x00])
    return list(dev.read(PACKET_LEN, timeout_ms=500))


def build_polling_packet(status_response):
    # OUT and IN of the polling exchange differ in only two bytes:
    #   - byte 1: 0x01 (sub-command "give me meter") in OUT
    #             0x00 (response type) in IN
    #   - byte 26: 0x00 in OUT, 0xff in IN (an amp-side flag)
    # The rest of bytes 0-44 must mirror the amp's reported state so the
    # firmware accepts the packet as in-sync. Bytes 45+ are amp-side
    # data, zeroed in OUT.
    out = list(status_response[:PACKET_LEN])
    if len(out) < PACKET_LEN:
        out += [0] * (PACKET_LEN - len(out))
    out[1] = 0x01
    out[26] = 0x00
    for i in range(45, PACKET_LEN):
        out[i] = 0x00
    return out


def linear_to_db(v, full_scale=32767.0):
    if v <= 0:
        return float("-inf")
    return 20.0 * math.log10(min(v, full_scale) / full_scale)


def db_bar(db, width=40, lo=DB_FLOOR, hi=0.0):
    if db == float("-inf"):
        frac = 0.0
    else:
        frac = max(0.0, min(1.0, (db - lo) / (hi - lo)))
    n = int(round(frac * width))
    return ("#" * n).ljust(width)


def main():
    dev = open_amp()
    status = get_status(dev)
    if not status or len(status) < PACKET_LEN:
        raise SystemExit("Couldn't read amp status to bootstrap.")

    preset = status[2]
    vol = int.from_bytes(bytes(status[3:5]), "little", signed=True) / 100.0
    print(f"# locked on FA503: preset={preset} vol={vol} dB",
          file=sys.stderr)
    print(f"# polling {POLL_HZ} Hz, Ctrl+C to stop\n", file=sys.stderr)

    packet = build_polling_packet(status)
    period = 1.0 / POLL_HZ

    peak_db = DB_FLOOR
    peak_t = 0.0

    try:
        while True:
            t0 = time.time()
            hidwrite(dev, packet)
            r = dev.read(PACKET_LEN, timeout_ms=500)
            if not r or len(r) < 50:
                print("(no response)")
                time.sleep(period)
                continue

            # Bytes 47-48 LE16 = our primary VU level.
            raw = r[47] | (r[48] << 8)
            db = linear_to_db(raw)

            now = time.time()
            if db > peak_db or (now - peak_t) > PEAK_HOLD_S:
                peak_db = db
                peak_t = now

            bar = db_bar(db)
            peak_pos = int(round(max(0.0, min(1.0, (peak_db - DB_FLOOR) / -DB_FLOOR)) * len(bar)))
            peak_pos = min(peak_pos, len(bar) - 1)
            bar_with_peak = bar[:peak_pos] + "|" + bar[peak_pos+1:]

            extra_b46 = r[46]
            extra_b49 = r[49]
            flag60 = r[60]

            sys.stdout.write(
                f"\r{db:6.1f} dB  [{bar_with_peak}]  "
                f"raw47-48={raw:>5}  b46={extra_b46:>3}  b49={extra_b49:>2}  "
                f"flag60=0x{flag60:02x}  "
            )
            sys.stdout.flush()

            elapsed = time.time() - t0
            if elapsed < period:
                time.sleep(period - elapsed)
    except KeyboardInterrupt:
        print("\nstopped.", file=sys.stderr)
    finally:
        dev.close()


if __name__ == "__main__":
    main()
