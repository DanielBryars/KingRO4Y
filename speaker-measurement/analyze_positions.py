"""Multi-position analysis: overlay all mic positions per speaker + spatial average.
Stable features across positions = the speaker; features that wander = the room.
Reads pos*_L.npy (repaired) and pos*_R.npy (good)."""
import glob, re, numpy as np
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
    out -= np.mean(out[(fb>=300)&(fb<=3000)])
    return fb, out

def load_set(side):
    files = sorted(glob.glob(f'pos*_{side}.npy'), key=lambda p:int(re.search(r'pos(\d+)_',p).group(1)))
    curves=[];
    for f in files:
        fb, m = gated(f); curves.append(m)
    return fb, np.array(curves), files

fbL, L, fL = load_set('L')   # repaired
fbR, R, fR = load_set('R')   # good
nL, nR = len(fL), len(fR)
print(f'LEFT/repaired positions: {nL}   RIGHT/good positions: {nR}')
avgL = L.mean(0) if nL else None
avgR = R.mean(0) if nR else None

majf=[20,30,40,50,60,80,100,200,300,400,500,600,800,1000,2000,3000,4000,5000,6000,8000,10000,15000,20000]
labels=['%dk'%(f/1000) if f>=1000 else str(f) for f in majf]
minf=[i*10 for i in range(2,10)]+[i*100 for i in range(2,10)]+[i*1000 for i in range(2,20)]
def axfmt(ax):
    ax.set_xlim(20,20000); ax.set_xticks(majf); ax.set_xticklabels(labels,fontsize=7,rotation=45)
    ax.xaxis.set_minor_locator(FixedLocator(minf)); ax.xaxis.set_minor_formatter(NullFormatter())
    ax.grid(True,which='major',alpha=0.35); ax.grid(True,which='minor',axis='x',alpha=0.12); ax.set_ylim(-25,12)

fig,(a1,a2,a3)=plt.subplots(3,1,figsize=(11,11),sharex=True)
for i,m in enumerate(L): a1.semilogx(fbL,m,color='tab:purple',alpha=0.3,lw=0.9)
if nL: a1.semilogx(fbL,avgL,color='tab:purple',lw=2.2,label=f'avg of {nL}')
a1.set_title('REPAIRED (left ch) — all positions + average'); a1.legend(loc='lower center'); a1.set_ylabel('dB')
for i,m in enumerate(R): a2.semilogx(fbR,m,color='tab:green',alpha=0.3,lw=0.9)
if nR: a2.semilogx(fbR,avgR,color='tab:green',lw=2.2,label=f'avg of {nR}')
a2.set_title('GOOD (right ch) — all positions + average'); a2.legend(loc='lower center'); a2.set_ylabel('dB')
if nL and nR:
    a3.semilogx(fbL,avgL,color='tab:purple',lw=2.0,label='repaired (avg)')
    a3.semilogx(fbR,avgR,color='tab:green',lw=2.0,label='good (avg)')
    a3.set_title('Spatial average: repaired vs good'); a3.legend(loc='lower center'); a3.set_ylabel('dB')
a3.set_xlabel('Frequency (Hz)')
for ax in (a1,a2,a3): axfmt(ax); ax.axhline(0,color='k',alpha=0.2)
plt.tight_layout(); plt.savefig('positions_compare.png',dpi=110); print('Saved: positions_compare.png')

if nL and nR:
    print('\n freq    rep(avg) good(avg)  diff   | rep spread(±) good spread(±)')
    for fc in [315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000]:
        j=np.argmin(np.abs(fbL-fc))
        print(f'{fc:6d}  {avgL[j]:7.1f} {avgR[j]:7.1f}  {avgL[j]-avgR[j]:+5.1f}   |   {L[:,j].std():5.1f}       {R[:,j].std():5.1f}')
    mb=(fbL>=300)&(fbL<=6000)
    print(f'\nAvg repaired-good (300Hz-6kHz): {np.mean(avgL[mb]-avgR[mb]):+.1f} dB  |  RMS diff: {np.sqrt(np.mean((avgL[mb]-avgR[mb])**2)):.1f} dB')
    print('(High per-position spread at a freq = room-dominated there; low spread = real speaker feature)')
