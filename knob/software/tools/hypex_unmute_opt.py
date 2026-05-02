"""Switch input to OPT (optical SPDIF) and unmute. Volume kept at current -60 dB."""
import hid

VID, PID = 0x345e, 0x03e8


def main():
    dev = hid.device()
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev.open_path(d["path"])
            break
    dev.set_nonblocking(False)

    dev.write([0x00, 0x06, 0x02, 0x00, 0x00] + [0] * 60)
    r = dev.read(64, timeout_ms=1000)
    cur_preset = r[2]
    cur_vol = int.from_bytes(bytes(r[3:5]), "little", signed=True)
    print(f"before: preset={cur_preset}  volume={cur_vol/100.0} dB  "
          f"muted={bool(r[6] & 0x80)}")

    cmd = [0x00,
           0x05,
           0x06,                                    # input: OPT
           cur_preset,
           cur_vol & 0xff, (cur_vol >> 8) & 0xff,
           0x00,
           0x00,                                    # mute OFF
           ]
    cmd += [0] * (65 - len(cmd))
    dev.write(cmd)
    r = dev.read(64, timeout_ms=1000)
    print(f"after:  preset={r[2]}  "
          f"volume={int.from_bytes(bytes(r[3:5]),'little',signed=True)/100.0} dB  "
          f"muted={bool(r[6] & 0x80)}")
    dev.close()


if __name__ == "__main__":
    main()
