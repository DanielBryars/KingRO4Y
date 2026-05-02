"""
Diagnostic — try several HID transfer styles to see which one the FA503 honours.
"""
import hid

VID, PID = 0x345e, 0x03e8


def open_dev():
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev = hid.device()
            dev.open_path(d["path"])
            dev.set_nonblocking(False)
            return dev
    raise SystemExit("no Hypex vendor interface found")


def hexdump(label, data):
    if not data:
        print(f"{label}: <empty>")
        return
    s = " ".join(f"{b:02x}" for b in data[:32])
    print(f"{label} ({len(data)}b): {s}{' ...' if len(data) > 32 else ''}")


def try_get_feature(dev, report_id, length=64):
    print(f"\n-- get_feature_report(0x{report_id:02x}, {length}) --")
    try:
        r = dev.get_feature_report(report_id, length)
        hexdump("  resp", r)
    except Exception as e:
        print(f"  error: {e}")


def try_send_feature_then_read(dev, payload):
    print(f"\n-- send_feature_report({[hex(b) for b in payload[:4]]}) then read --")
    try:
        n = dev.send_feature_report(payload + [0] * (64 - len(payload)))
        print(f"  sent {n} bytes")
    except Exception as e:
        print(f"  send error: {e}")
        return
    try:
        r = dev.read(64, timeout_ms=500)
        hexdump("  read", r)
    except Exception as e:
        print(f"  read error: {e}")


def try_write_then_read(dev, payload):
    print(f"\n-- write({[hex(b) for b in payload[:4]]}) then read --")
    try:
        n = dev.write(payload + [0] * (64 - len(payload)))
        print(f"  wrote {n} bytes")
    except Exception as e:
        print(f"  write error: {e}")
        return
    try:
        r = dev.read(64, timeout_ms=500)
        hexdump("  read", r)
    except Exception as e:
        print(f"  read error: {e}")


def main():
    dev = open_dev()
    print(f"manufacturer: {dev.get_manufacturer_string()}")
    print(f"product:      {dev.get_product_string()}")
    print(f"serial:       {dev.get_serial_number_string()}")

    # Try interpreting "06 02 00 00 ..." as: Feature Report ID 0x06, sub=0x02
    try_get_feature(dev, 0x06)
    try_get_feature(dev, 0x05)
    try_get_feature(dev, 0x03)

    try_send_feature_then_read(dev, [0x06, 0x02, 0x00, 0x00])
    try_send_feature_then_read(dev, [0x03, 0x08, 0x00, 0x00])

    try_write_then_read(dev, [0x06, 0x02, 0x00, 0x00])

    # NB: a Set State write (opcode 0x05) with zero fields will commit zeros
    # to the amp — preset 0, 0 dB, unmuted. That's destructive and can be
    # very loud. The original version of this diagnostic ran one and zeroed
    # a live amp on 2026-05-02. Use a non-zero, round-tripped Set State or
    # `hypex_probe.py set-volume` for any write tests instead.
    # try_write_then_read(dev, [0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])

    # Try with leading 0x00 = "no report ID" — hidapi strips it before send.
    try_write_then_read(dev, [0x00, 0x06, 0x02, 0x00, 0x00])
    # try_write_then_read(dev, [0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])

    # Just read with no prior write — maybe it streams unsolicited status.
    print("\n-- bare read (no prior write) --")
    r = dev.read(64, timeout_ms=500)
    hexdump("  read", r)

    dev.close()


if __name__ == "__main__":
    main()
