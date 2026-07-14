"""Analyze saved impulse response -> magnitude, minimum-phase, polarity. Regenerates the plot."""
import numpy as np
from scipy.signal import hilbert
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt

fs = 48000
ir = np.load('ir.npy')
pk = int(np.argmax(np.abs(ir)))
polarity = '+ (normal)' if ir[pk] > 0 else '- (INVERTED)'
print(f'IR peak sample {pk}, polarity {polarity}')

# window the direct sound (5 ms pre, 50 ms post) for the transfer function
w0 = max(0, pk-int(0.005*fs)); w1 = pk+int(0.05*fs)
irw = ir[w0:w1]*np.hanning(w1-w0)
NF = 1<<16
H = np.fft.rfft(irw, NF)
freq = np.fft.rfftfreq(NF, 1/fs)
mag = 20*np.log10(np.abs(H)+1e-12)

def smooth(freq, y, frac=6):
    out = np.copy(y)
    for i, f in enumerate(freq):
        if f <= 0: continue
        lo, hi = f/2**(1/(2*frac)), f*2**(1/(2*frac))
        m = (freq >= lo) & (freq <= hi)
        if m.any(): out[i] = np.mean(y[m])
    return out

band = (freq >= 15) & (freq <= 22000); fb = freq[band]
mags = smooth(fb, mag[band]); ref = (fb >= 200) & (fb <= 2000)
mags -= np.mean(mags[ref])

# minimum-phase from the (smoothed) magnitude via Hilbert transform of log-magnitude
logmag = np.log(np.maximum(10**(smooth(freq, mag)/20), 1e-6))
minphase = -np.imag(hilbert(logmag))          # radians
mp = np.degrees(minphase[band])
show = (fb >= 30) & (fb <= 18000)

fig, (a1, a2) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
a1.semilogx(fb, mags, color='tab:blue', lw=1.4)
a1.set_ylabel('Magnitude (dB)')
a1.set_title(f'Single speaker (LEFT) — in-room log-sweep,  polarity {polarity}')
a1.grid(True, which='both', alpha=0.3); a1.set_ylim(-30, 15)
a1.axhline(0, color='k', alpha=0.2)
a2.semilogx(fb[show], mp[show], color='tab:green', lw=1.4)
a2.set_ylabel('Min-phase (deg)'); a2.set_xlabel('Frequency (Hz)')
a2.grid(True, which='both', alpha=0.3); a2.set_xlim(20, 20000); a2.set_ylim(-200, 200)
plt.tight_layout(); plt.savefig('speaker_response.png', dpi=110)
print('Saved plot: speaker_response.png')

# summary numbers
def corner(fb, mags, side):
    idx = np.where((fb >= 40) & (fb <= 20000))[0]
    for j in (idx if side == 'low' else idx[::-1]):
        if mags[j] >= -6: return fb[j]
inb = (fb >= 100) & (fb <= 10000)
print(f'-6 dB band: {corner(fb,mags,"low"):.0f} Hz .. {corner(fb,mags,"high"):.0f} Hz')
print(f'100 Hz-10 kHz ripple: +/-{(np.max(mags[inb])-np.min(mags[inb]))/2:.1f} dB')
