"""Record ambient from the Scarlett across host APIs; report per-channel levels."""
import numpy as np, sounddevice as sd
from audio_lib import dbfs

devs = sd.query_devices(); apis = sd.query_hostapis()
targets = []
for i,d in enumerate(devs):
    if 'scarlett' in d['name'].lower() and d['max_input_channels']>=1:
        targets.append((i, apis[d['hostapi']]['name'], d['max_input_channels'], int(d['default_samplerate'])))

for i, api, ch, sr in targets:
    try:
        rec = sd.rec(int(2*sr), samplerate=sr, channels=ch, device=i, dtype='float32', blocking=True)
        peak = 20*np.log10(np.max(np.abs(rec))+1e-12)
        rms = '  '.join(f'ch{c+1}={dbfs(rec[:,c]):6.1f}' for c in range(ch))
        print(f'[{i:2d}] {api:16s} sr={sr} peak={peak:6.1f} dBFS | {rms}')
    except Exception as e:
        print(f'[{i:2d}] {api:16s} ERROR: {e}')
