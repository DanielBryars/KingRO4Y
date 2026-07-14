"""Compare LEFT vs RIGHT speaker (same mic position, same preset).
Preserves the real level difference (no independent per-curve normalization)."""
import numpy as np
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullFormatter

fs = 48000

def gated(path, gate_ms=5.0, frac=6):
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
        if m.any(): out[i]=np.mean(mag[m])
    band=(f>=15)&(f<=22000); return f[band], out[band], pk

fb, L, pkL = gated('ir_left.npy')
_,  R, pkR = gated('ir_right.npy')
# common reference: set LEFT mean(300-3k)=0, apply SAME offset to R  -> R shows true level vs L
ref=(fb>=300)&(fb<=3000); off=np.mean(L[ref]); L-=off; R-=off
diff = L-R

fig,(a1,a2)=plt.subplots(2,1,figsize=(11,7.5),sharex=True,gridspec_kw={'height_ratios':[2,1]})
a1.semilogx(fb,L,color='tab:blue',lw=1.6,label='LEFT')
a1.semilogx(fb,R,color='tab:red',lw=1.6,label='RIGHT')
a1.set_ylabel('Magnitude (dB, common ref)'); a1.set_ylim(-25,12); a1.axhline(0,color='k',alpha=0.2)
a1.legend(loc='lower center'); a1.set_title('LEFT vs RIGHT — both preset 3, same mic position (trust >300 Hz)')
a2.semilogx(fb,diff,color='0.3',lw=1.3); a2.axhline(0,color='k',alpha=0.3)
for y in (-3,3): a2.axhline(y,color='tab:orange',ls='--',alpha=0.5)
a2.set_ylabel('L − R (dB)'); a2.set_xlabel('Frequency (Hz)'); a2.set_ylim(-12,12)
majf=[20,30,40,50,60,80,100,200,300,400,500,600,800,1000,2000,3000,4000,5000,6000,8000,10000,15000,20000]
labels=['%dk'%(f/1000) if f>=1000 else str(f) for f in majf]
minf=[i*10 for i in range(2,10)]+[i*100 for i in range(2,10)]+[i*1000 for i in range(2,20)]
for ax in (a1,a2):
    ax.set_xlim(20,20000); ax.set_xticks(majf); ax.set_xticklabels(labels,fontsize=7,rotation=45)
    ax.xaxis.set_minor_locator(FixedLocator(minf)); ax.xaxis.set_minor_formatter(NullFormatter())
    ax.grid(True,which='major',alpha=0.35); ax.grid(True,which='minor',axis='x',alpha=0.12)
plt.tight_layout(); plt.savefig('speaker_compare.png',dpi=110); print('Saved: speaker_compare.png')

print('\n freq     LEFT   RIGHT   L-R')
for fc in [100,160,200,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000]:
    j=np.argmin(np.abs(fb-fc)); print(f'{fc:6d}  {L[j]:6.1f}  {R[j]:6.1f}  {diff[j]:+6.1f}')
mb=(fb>=300)&(fb<=6000)
print(f'\nMean L-R (300Hz-6kHz): {np.mean(diff[mb]):+.1f} dB   |  RMS of L-R: {np.sqrt(np.mean(diff[mb]**2)):.1f} dB')
print(f'Direct-sound peak samples: L={pkL}  R={pkR}  (per-run offset differs; not a distance metric)')
