"""Driver-vs-driver comparison using the swap: good vs repaired at each fixed position.
Same position => same room, so shape differences are the DRIVER, not the room.
(Volume changed between runs, so compare shape: each curve normalized to its own 300-3k mean.)"""
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
        lo, hi = fq/2**(1/(2*frac)), fq*2**(1/(2*frac)); m=(f>=lo)&(f<=hi)
        if m.any(): out[i]=np.mean(mag[m])
    band=(f>=15)&(f<=22000); fb=f[band]; out=out[band]
    out -= np.mean(out[(fb>=300)&(fb<=3000)])   # own-level normalize (shape only)
    return fb, out

def polarity_xcorr(p_good, p_rep):
    a=np.load(p_good); b=np.load(p_rep)
    def dwin(ir):
        pk=int(np.argmax(np.abs(ir))); s=ir[pk-48:pk+240].astype(float)
        return s/ (np.linalg.norm(s)+1e-12)
    da, db = dwin(a), dwin(b)
    c=np.correlate(da, db, 'full'); k=int(np.argmax(np.abs(c)))
    return c[k]   # sign: + same polarity, - opposite

fbL, gL = gated('good_Lpos.npy');      _, rL = gated('repaired_Lpos.npy')   # LEFT position
fbR, rR = gated('repaired_Rpos.npy');  _, gR = gated('good_Rpos.npy')       # RIGHT position

majf=[20,30,40,50,60,80,100,200,300,400,500,600,800,1000,2000,3000,4000,5000,6000,8000,10000,15000,20000]
labels=['%dk'%(f/1000) if f>=1000 else str(f) for f in majf]
minf=[i*10 for i in range(2,10)]+[i*100 for i in range(2,10)]+[i*1000 for i in range(2,20)]
def axfmt(ax):
    ax.set_xlim(20,20000); ax.set_xticks(majf); ax.set_xticklabels(labels,fontsize=7,rotation=45)
    ax.xaxis.set_minor_locator(FixedLocator(minf)); ax.xaxis.set_minor_formatter(NullFormatter())
    ax.grid(True,which='major',alpha=0.35); ax.grid(True,which='minor',axis='x',alpha=0.12)

fig,(a1,a2)=plt.subplots(2,1,figsize=(11,8),sharex=True)
a1.semilogx(fbL,gL,color='tab:green',lw=1.6,label='GOOD driver')
a1.semilogx(fbL,rL,color='tab:purple',lw=1.6,label='REPAIRED driver')
a1.set_title('LEFT position (room fixed) — good vs repaired driver'); a1.set_ylim(-25,12)
a1.axhline(0,color='k',alpha=0.2); a1.legend(loc='lower center'); a1.set_ylabel('dB (shape)')
a2.semilogx(fbR,gR,color='tab:green',lw=1.6,label='GOOD driver')
a2.semilogx(fbR,rR,color='tab:purple',lw=1.6,label='REPAIRED driver')
a2.set_title('RIGHT position (room fixed) — good vs repaired driver'); a2.set_ylim(-25,12)
a2.axhline(0,color='k',alpha=0.2); a2.legend(loc='lower center'); a2.set_ylabel('dB (shape)'); a2.set_xlabel('Frequency (Hz)')
axfmt(a1); axfmt(a2)
plt.tight_layout(); plt.savefig('swap_compare.png',dpi=110); print('Saved: swap_compare.png')

print('\n            LEFT pos (good-rep)   RIGHT pos (good-rep)')
for fc in [315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000]:
    jL=np.argmin(np.abs(fbL-fc)); jR=np.argmin(np.abs(fbR-fc))
    print(f'{fc:6d} Hz     {gL[jL]-rL[jL]:+6.1f}              {gR[jR]-rR[jR]:+6.1f}')

cL=polarity_xcorr('good_Lpos.npy','repaired_Lpos.npy')
cR=polarity_xcorr('good_Rpos.npy','repaired_Rpos.npy')
print(f'\nPolarity xcorr (good vs repaired): LEFT pos {cL:+.2f}, RIGHT pos {cR:+.2f}'
      f'  ({"OPPOSITE polarity" if (cL<0 and cR<0) else "same polarity" if (cL>0 and cR>0) else "inconclusive"})')
