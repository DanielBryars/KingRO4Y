"""Overlay gated (quasi-anechoic) responses of preset 1 (ir.npy) vs preset 2 (ir_preset2.npy)."""
import numpy as np
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullFormatter

fs = 48000

def gated_response(path, gate_ms=5.0, frac=6):
    ir = np.load(path); pk = int(np.argmax(np.abs(ir)))
    w0 = max(0, pk-int(0.001*fs)); w1 = pk+int(gate_ms/1000*fs)
    seg = ir[w0:w1].astype(float).copy()
    tail = int(0.25*len(seg)); win = np.ones(len(seg)); win[-tail:] = np.hanning(2*tail)[tail:]
    seg *= win
    NF = 1<<16; H = np.fft.rfft(seg, NF); f = np.fft.rfftfreq(NF, 1/fs)
    mag = 20*np.log10(np.abs(H)+1e-12); out = np.copy(mag)
    for i, fq in enumerate(f):
        if fq<=0: continue
        lo, hi = fq/2**(1/(2*frac)), fq*2**(1/(2*frac)); m = (f>=lo)&(f<=hi)
        if m.any(): out[i] = np.mean(mag[m])
    band = (f>=15)&(f<=22000); fb = f[band]; out = out[band]
    ref = (fb>=500)&(fb<=2000); out -= np.mean(out[ref])
    return fb, out

fb, p3 = gated_response('ir.npy')          # preset 3 (current)
_,  p1 = gated_response('ir_preset1.npy')  # preset 1
_,  p2 = gated_response('ir_preset2.npy')  # preset 2

fig, ax = plt.subplots(figsize=(11,5.5))
ax.semilogx(fb, p1, color='tab:blue',   lw=1.6, label='preset 1')
ax.semilogx(fb, p2, color='tab:orange', lw=1.6, label='preset 2')
ax.semilogx(fb, p3, color='tab:green',  lw=1.6, label='preset 3')
ax.set_ylabel('Magnitude (dB)'); ax.set_xlabel('Frequency (Hz)')
ax.set_title('Gated (quasi-anechoic) response — presets 1/2/3  (trust >300 Hz)')
ax.set_ylim(-25,12); ax.set_xlim(20,20000); ax.axhline(0,color='k',alpha=0.2)
majf = [20,30,40,50,60,80,100,200,300,400,500,600,800,1000,2000,3000,4000,5000,6000,8000,10000,15000,20000]
labels = ['%dk'%(f/1000) if f>=1000 else str(f) for f in majf]
minf = [i*10 for i in range(2,10)]+[i*100 for i in range(2,10)]+[i*1000 for i in range(2,20)]
ax.set_xticks(majf); ax.set_xticklabels(labels, fontsize=7, rotation=45)
ax.xaxis.set_minor_locator(FixedLocator(minf)); ax.xaxis.set_minor_formatter(NullFormatter())
ax.grid(True, which='major', alpha=0.35); ax.grid(True, which='minor', axis='x', alpha=0.12)
ax.legend(loc='lower center')
plt.tight_layout(); plt.savefig('preset_compare.png', dpi=110)
print('Saved: preset_compare.png')

# level table
print('\n freq   preset1  preset2  preset3')
for fc in [100,160,200,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000]:
    j = np.argmin(np.abs(fb-fc))
    print(f'{fc:6d}  {p1[j]:7.1f}  {p2[j]:7.1f}  {p3[j]:7.1f}')
