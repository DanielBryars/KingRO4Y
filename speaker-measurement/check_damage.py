"""Damage check: compare a fresh capture against a known-good reference capture.
Checks (a) gated frequency response, (b) harmonic distortion (H2..H5).

In a Farina log sweep the harmonic products appear as separate impulses BEFORE the
linear IR, at dt_n = T*ln(n)/ln(f2/f1) ahead of it. Elevated harmonics vs the
reference = mechanical damage (rub/buzz/loose joint), even if the FR looks normal.

Usage: python check_damage.py reference.npy new.npy
"""
import sys, numpy as np
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullFormatter

fs = 48000
T, f1, f2 = 6.0, 20.0, 20000.0        # must match capture_channel.py
Lsw = np.log(f2/f1)

ref_path = sys.argv[1] if len(sys.argv) > 1 else 'onaxis_R.npy'
new_path = sys.argv[2] if len(sys.argv) > 2 else 'onaxis_R_new.npy'

def gated(ir, pk, gate_ms=5.0, frac=6):
    w0 = max(0, pk-int(0.001*fs)); w1 = pk+int(gate_ms/1000*fs)
    seg = ir[w0:w1].astype(float).copy()
    tail = int(0.25*len(seg)); win = np.ones(len(seg)); win[-tail:] = np.hanning(2*tail)[tail:]
    seg *= win
    NF = 1<<16; H = np.fft.rfft(seg, NF); f = np.fft.rfftfreq(NF, 1/fs)
    mag = 20*np.log10(np.abs(H)+1e-12); out = np.copy(mag)
    for i, fq in enumerate(f):
        if fq <= 0: continue
        lo, hi = fq/2**(1/(2*frac)), fq*2**(1/(2*frac)); m = (f>=lo)&(f<=hi)
        if m.any(): out[i] = np.mean(mag[m])
    band = (f>=15)&(f<=22000)
    return f[band], out[band]

def harmonics(ir, pk, half_ms=8.0):
    """Energy of linear IR and of each harmonic impulse, in dB relative to linear."""
    h = int(half_ms/1000*fs)
    lin = ir[pk-h:pk+h]
    e_lin = np.sum(lin**2) + 1e-30
    out = {}
    for n in (2,3,4,5):
        dt = int(T*np.log(n)/Lsw*fs)
        c = pk - dt
        if c-h < 0: out[n] = None; continue
        seg = ir[c-h:c+h]
        out[n] = 10*np.log10((np.sum(seg**2)+1e-30)/e_lin)
    # noise floor estimate: a quiet region well away from any harmonic
    nfc = pk - int(T*np.log(2.5)/Lsw*fs)
    noise = ir[nfc-h:nfc+h]
    out['noise'] = 10*np.log10((np.sum(noise**2)+1e-30)/e_lin)
    return out

res = {}
for tag, path in (('reference', ref_path), ('new', new_path)):
    ir = np.load(path); pk = int(np.argmax(np.abs(ir)))
    fb, mg = gated(ir, pk)
    hm = harmonics(ir, pk)
    res[tag] = dict(fb=fb, mag=mg, h=hm, pk=pk, path=path)
    print(f'{tag:9s} {path:22s} IR peak |{abs(ir[pk]):.3g}|')

# normalize each FR to its own 300-3k mean (drive level may differ) -> compare SHAPE
for t in res:
    fb = res[t]['fb']; m = res[t]['mag']
    res[t]['mag'] = m - np.mean(m[(fb>=300)&(fb<=3000)])

fb = res['reference']['fb']
d = res['new']['mag'] - res['reference']['mag']

print('\n--- FREQUENCY RESPONSE (shape, new - reference) ---')
for fc in [100,200,315,500,800,1000,1600,2000,3150,4000,5000,6300,8000,10000,12500,16000]:
    j = np.argmin(np.abs(fb-fc))
    flag = '  <-- CHECK' if abs(d[j]) > 3 else ''
    print(f'{fc:6d} Hz  {d[j]:+6.1f} dB{flag}')
mb = (fb>=300)&(fb<=10000)
print(f'\nMean |new-ref| 300Hz-10kHz: {np.mean(np.abs(d[mb])):.1f} dB   max: {np.max(np.abs(d[mb])):.1f} dB')

print('\n--- HARMONIC DISTORTION (dB relative to linear IR; lower = cleaner) ---')
print('         reference     new      change')
for n in (2,3,4,5):
    a, b = res['reference']['h'][n], res['new']['h'][n]
    if a is None or b is None: continue
    ch = b-a
    flag = '  <-- RAISED' if ch > 6 else ''
    print(f'  H{n}    {a:8.1f}  {b:8.1f}   {ch:+6.1f}{flag}')
a, b = res['reference']['h']['noise'], res['new']['h']['noise']
print(f'  noise {a:8.1f}  {b:8.1f}   {b-a:+6.1f}   (floor reference)')

# plot
majf=[20,30,40,50,60,80,100,200,300,400,500,600,800,1000,2000,3000,4000,5000,6000,8000,10000,15000,20000]
labels=['%dk'%(f/1000) if f>=1000 else str(f) for f in majf]
minf=[i*10 for i in range(2,10)]+[i*100 for i in range(2,10)]+[i*1000 for i in range(2,20)]
fig,(a1,a2)=plt.subplots(2,1,figsize=(11,7.5),sharex=True,gridspec_kw={'height_ratios':[2,1]})
a1.semilogx(fb,res['reference']['mag'],color='tab:green',lw=1.6,label='reference (healthy)')
a1.semilogx(fb,res['new']['mag'],color='tab:red',lw=1.4,label='now (after full volume)')
a1.set_ylabel('Magnitude (dB)'); a1.set_ylim(-25,15); a1.legend(loc='lower center'); a1.axhline(0,color='k',alpha=0.2)
a1.set_title('RIGHT speaker — damage check vs known-good reference (on-axis 95 cm)')
a2.semilogx(fb,d,color='0.3',lw=1.3); a2.axhline(0,color='k',alpha=0.3)
for y in (-3,3): a2.axhline(y,color='tab:orange',ls='--',alpha=0.6)
a2.set_ylabel('new - ref (dB)'); a2.set_xlabel('Frequency (Hz)'); a2.set_ylim(-12,12)
for ax in (a1,a2):
    ax.set_xlim(20,20000); ax.set_xticks(majf); ax.set_xticklabels(labels,fontsize=7,rotation=45)
    ax.xaxis.set_minor_locator(FixedLocator(minf)); ax.xaxis.set_minor_formatter(NullFormatter())
    ax.grid(True,which='major',alpha=0.35); ax.grid(True,which='minor',axis='x',alpha=0.12)
plt.tight_layout(); plt.savefig('damage_check.png',dpi=110); print('\nSaved: damage_check.png')
