"""Connectivity + channel-map sanity check: tone on L, then R, while recording the mic."""
import numpy as np, sounddevice as sd
from audio_lib import find_device, play_record, dbfs, FS

in_dev, ind, in_api = find_device('Scarlett', 'input')
out_dev, outd, out_api = find_device('Digital Output (Realtek', 'output')
out_ch = min(2, outd['max_output_channels'])
print(f'INPUT : [{in_dev}] {ind["name"]}  ({in_api})')
print(f'OUTPUT: [{out_dev}] {outd["name"]}  ({out_api}), using {out_ch}ch')

fs = FS
seg = int(1.0 * fs)
gap = int(0.4 * fs)
amp = 10**(-12/20)  # -12 dBFS, moderate
t = np.arange(seg)/fs
tone = (amp*np.sin(2*np.pi*1000*t)).astype('float32')
# fade to avoid clicks
fade = int(0.01*fs); env = np.ones(seg); env[:fade]=np.linspace(0,1,fade); env[-fade:]=np.linspace(1,0,fade)
tone *= env

sig = np.zeros((gap+seg+gap+seg+gap, out_ch), dtype='float32')
p = gap
sig[p:p+seg, 0] = tone          # LEFT segment
pL = (p, p+seg)
p += seg+gap
if out_ch > 1:
    sig[p:p+seg, 1] = tone      # RIGHT segment
pR = (p, p+seg)

print('Playing L tone, then R tone...')
rec = play_record(sig, in_dev, out_dev, in_channels=min(2, ind['max_input_channels']), fs=fs)

# playback started ~150ms after record; find offset by max energy, but simpler: just window generously
lead = int(0.15*fs)
def seg_rms(rng, ch):
    a, b = rng[0]+lead, rng[1]+lead
    return dbfs(rec[a:b, ch])
nch = rec.shape[1]
print(f'\nRecorded {nch} mic channel(s). Overall peak = {20*np.log10(np.max(np.abs(rec))+1e-12):.1f} dBFS')
print('\n            ', '  '.join(f'mic{c+1:d}' for c in range(nch)))
print('L-tone seg :', '  '.join(f'{seg_rms(pL,c):6.1f}' for c in range(nch)), 'dBFS RMS')
print('R-tone seg :', '  '.join(f'{seg_rms(pR,c):6.1f}' for c in range(nch)), 'dBFS RMS')
# silence baseline (first gap)
print('silence    :', '  '.join(f'{dbfs(rec[:gap-lead,c]):6.1f}' for c in range(nch)), 'dBFS RMS')
