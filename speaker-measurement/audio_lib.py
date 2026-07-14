"""Shared audio helpers: pick devices, run concurrent play+record across two devices."""
import sys, threading
import numpy as np
import sounddevice as sd

FS = 48000

def find_device(name_substr, kind, hostapi_pref=('Windows WASAPI', 'MME')):
    """kind='input' or 'output'. Prefer given host APIs in order."""
    devs = sd.query_devices()
    apis = sd.query_hostapis()
    cand = []
    for i, d in enumerate(devs):
        if name_substr.lower() not in d['name'].lower():
            continue
        if kind == 'input' and d['max_input_channels'] < 1:
            continue
        if kind == 'output' and d['max_output_channels'] < 1:
            continue
        cand.append((i, apis[d['hostapi']]['name'], d))
    for pref in hostapi_pref:
        for i, api, d in cand:
            if api == pref:
                return i, d, api
    if cand:
        i, api, d = cand[0]
        return i, d, api
    raise RuntimeError(f'No {kind} device matching "{name_substr}"')

def play_record(out_sig, in_dev, out_dev, in_channels=2, fs=FS):
    """Play out_sig (frames x out_ch) on out_dev while recording in_channels from in_dev.
    Two separate blocking streams in threads. Recording is padded so it fully brackets playback."""
    out_sig = np.ascontiguousarray(out_sig, dtype='float32')
    n = len(out_sig)
    out_ch = out_sig.shape[1]
    rec_frames = n + int(0.5 * fs)  # tail margin
    recorded = np.zeros((rec_frames, in_channels), dtype='float32')
    rec_done = threading.Event()

    def record():
        with sd.InputStream(samplerate=fs, device=in_dev, channels=in_channels,
                            dtype='float32', blocksize=1024) as ins:
            idx = 0
            while idx < rec_frames:
                block, _ = ins.read(min(1024, rec_frames - idx))
                recorded[idx:idx+len(block)] = block
                idx += len(block)
        rec_done.set()

    def play():
        # small lead so recorder is running first
        sd.sleep(150)
        with sd.OutputStream(samplerate=fs, device=out_dev, channels=out_ch,
                             dtype='float32', blocksize=1024) as outs:
            outs.write(out_sig)

    rt = threading.Thread(target=record)
    pt = threading.Thread(target=play)
    rt.start(); pt.start()
    pt.join(); rt.join()
    return recorded

def dbfs(x):
    r = np.sqrt(np.mean(x**2)) + 1e-12
    return 20*np.log10(r)
