"""
Tkinter slider for Hypex FA503 volume.

Reads current state on launch, then live-writes volume as the slider moves,
with a short debounce so we don't pummel the USB endpoint while dragging.
Mute button included. Preset/input/mute fields are round-tripped so we never
clobber them.
"""
import tkinter as tk
from tkinter import ttk

import hid

VID, PID = 0x345e, 0x03e8
PACKET_LEN = 64
DEBOUNCE_MS = 30
VOL_MIN_DB = -80.0
VOL_MAX_DB = 0.0


def open_amp():
    for d in hid.enumerate(VID, PID):
        if d["usage_page"] >= 0xff00:
            dev = hid.device()
            dev.open_path(d["path"])
            dev.set_nonblocking(False)
            return dev
    raise SystemExit("No Hypex amp found.")


def pad(payload):
    return [0x00] + list(payload) + [0] * (PACKET_LEN - len(payload))


def get_status(dev):
    dev.write(pad([0x06, 0x02, 0x00, 0x00]))
    return dev.read(PACKET_LEN, timeout_ms=500)


def set_state(dev, *, input_source, preset, volume_centidb, mute):
    vol = volume_centidb & 0xffff
    pkt = pad([0x05, input_source, preset,
               vol & 0xff, (vol >> 8) & 0xff,
               0x00, 0x80 if mute else 0x00])
    dev.write(pkt)
    return dev.read(PACKET_LEN, timeout_ms=500)


INPUT_NAMES = {0x00: "scan", 0x01: "XLR", 0x02: "RCA",
               0x04: "SPDIF", 0x05: "AES", 0x06: "OPT"}


class App:
    def __init__(self, root, dev):
        self.dev = dev
        self.root = root
        self.pending_after = None

        s = get_status(dev)
        self.preset = s[2]
        self.volume_centidb = int.from_bytes(bytes(s[3:5]), "little", signed=True)
        self.mute = bool(s[6] & 0x80)
        self.input_source = s[50] if len(s) > 50 else 0x00

        root.title("Hypex FA503 Volume")
        root.geometry("440x200")
        root.resizable(False, False)

        self.value_label = tk.Label(root, font=("Segoe UI", 28))
        self.value_label.pack(pady=(15, 0))

        self.scale_var = tk.DoubleVar(value=self.volume_centidb / 100.0)
        self.scale = ttk.Scale(
            root, from_=VOL_MIN_DB, to=VOL_MAX_DB, orient=tk.HORIZONTAL,
            length=400, variable=self.scale_var, command=self._on_slide,
        )
        self.scale.pack(pady=10, padx=20, fill=tk.X)

        bar = tk.Frame(root)
        bar.pack(pady=5)
        self.mute_btn = tk.Button(bar, text="Mute", width=10,
                                  command=self._toggle_mute)
        self.mute_btn.pack(side=tk.LEFT, padx=4)
        tk.Button(bar, text="Refresh", width=10,
                  command=self._refresh).pack(side=tk.LEFT, padx=4)

        self.status_label = tk.Label(root, font=("Consolas", 9), fg="#666")
        self.status_label.pack(pady=(8, 0))

        self._update_labels()
        root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _on_slide(self, value_str):
        self.volume_centidb = int(round(float(value_str) * 100))
        self.value_label.config(text=f"{self.volume_centidb/100.0:.1f} dB")
        # Coalesce rapid drag events into one write per ~30 ms idle.
        if self.pending_after is not None:
            self.root.after_cancel(self.pending_after)
        self.pending_after = self.root.after(DEBOUNCE_MS, self._send_state)

    def _send_state(self):
        self.pending_after = None
        try:
            r = set_state(self.dev,
                          input_source=0x00,    # 0x00 = "no change"
                          preset=self.preset,
                          volume_centidb=self.volume_centidb,
                          mute=self.mute)
            if r and len(r) > 6:
                self.mute = bool(r[6] & 0x80)
                if len(r) > 50:
                    self.input_source = r[50]
            self._update_labels()
        except Exception as e:
            self.status_label.config(text=f"error: {e}", fg="red")

    def _toggle_mute(self):
        self.mute = not self.mute
        self._send_state()

    def _refresh(self):
        try:
            s = get_status(self.dev)
            self.preset = s[2]
            self.volume_centidb = int.from_bytes(bytes(s[3:5]), "little",
                                                 signed=True)
            self.mute = bool(s[6] & 0x80)
            if len(s) > 50:
                self.input_source = s[50]
            self.scale_var.set(self.volume_centidb / 100.0)
            self._update_labels()
        except Exception as e:
            self.status_label.config(text=f"error: {e}", fg="red")

    def _update_labels(self):
        self.value_label.config(text=f"{self.volume_centidb/100.0:.1f} dB")
        self.mute_btn.config(text="Unmute" if self.mute else "Mute",
                             relief=tk.SUNKEN if self.mute else tk.RAISED)
        inp = INPUT_NAMES.get(self.input_source, f"0x{self.input_source:02x}")
        self.status_label.config(
            text=f"preset {self.preset}   input {inp}   "
                 f"{'MUTED' if self.mute else 'live'}",
            fg="#666",
        )

    def _on_close(self):
        try:
            self.dev.close()
        finally:
            self.root.destroy()


def main():
    dev = open_amp()
    root = tk.Tk()
    App(root, dev)
    root.mainloop()


if __name__ == "__main__":
    main()
