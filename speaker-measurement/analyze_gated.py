"""Quasi-anechoic (time-gated) analysis to separate the speaker from the room.
Compares an ungated response with a short gate that excludes the first reflection,
so 3-way crossover integration can be judged. Reads ir.npy."""
import numpy as np
from scipy.signal import hilbert
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt

fs = 48000
ir = np.load('ir.npy')
pk = int(np.argmax(np.abs(ir)))
print(f'IR peak sample {pk}, polarity {"+" if ir[pk]>0 else "-"}')

# find the first reflection: largest secondary peak after the direct sound
seg = ir[pk:pk+int(0.02*fs)]
env = np.abs(seg)
direct = env[0]
refl = None
for i in range(int(0.0005*fs), len(env)):          # skip 0.5 ms around direct
    if env[i] > 0.3*direct:
        refl = i; break
if refl:
    print(f'First strong reflection ~{refl/fs*1e3:.1f} ms after direct '
          f'(gate below this isolates the speaker; valid above ~{1000/(refl/fs*1e3):.0f} Hz)')

def response(gate_ms, frac=6):
    pre = int(0.001*fs)
    w0 = max(0, pk-pre)
    glen = int(gate_ms/1000*fs)
    w1 = pk+glen
    seg = ir[w0:w1].astype(float).copy()
    # half-Hann fade-out on the last 25% of the gate to avoid truncation ringing
    tail = int(0.25*len(seg)); win = np.ones(len(seg))
    win[-tail:] = np.hanning(2*tail)[tail:]
    seg *= win
    NF = 1<<16
    H = np.fft.rfft(seg, NF); f = np.fft.rfftfreq(NF, 1/fs)
    mag = 20*np.log10(np.abs(H)+1e-12)
    # smooth
    out = np.copy(mag)
    for i, fq in enumerate(f):
        if fq<=0: continue
        lo, hi = fq/2**(1/(2*frac)), fq*2**(1/(2*frac))
        m = (f>=lo)&(f<=hi)
        if m.any(): out[i]=np.mean(mag[m])
    return f, out, H

f, m_full, _ = response(50.0)       # essentially ungated (includes room)
f, m_gate, Hg = response(max(3.0, (refl/fs*1e3-0.5)) if refl else 5.0)  # gated to just before 1st reflection
band = (f>=15)&(f<=22000); fb=f[band]
ref = (fb>=500)&(fb<=2000)
m_full=m_full[band]-np.mean(m_full[band][ref])
m_gate=m_gate[band]-np.mean(m_gate[band][ref])

# minimum phase from the GATED magnitude
logmag = np.log(np.maximum(10**(m_gate/20),1e-6))
mp = np.degrees(-np.imag(hilbert(logmag)))

fig,(a1,a2)=plt.subplots(2,1,figsize=(10,7.5),sharex=True)
a1.semilogx(fb,m_full,color='0.7',lw=1.0,label='ungated (speaker + room)')
a1.semilogx(fb,m_gate,color='tab:blue',lw=1.6,label='gated (quasi-anechoic)')
a1.set_ylabel('Magnitude (dB)'); a1.set_ylim(-25,15); a1.grid(True,which='both',alpha=0.3)
a1.legend(loc='lower center',fontsize=8); a1.axhline(0,color='k',alpha=0.2)
a1.set_title('3-way in-room vs time-gated  (gated reveals crossover integration)')
show=(fb>=200)&(fb<=18000)
a2.semilogx(fb[show],mp[show],color='tab:green',lw=1.4)
a2.set_ylabel('Min-phase (deg)'); a2.set_xlabel('Frequency (Hz)')
a2.set_xlim(20,20000); a2.set_ylim(-200,200); a2.grid(True,which='both',alpha=0.3)

# detailed frequency x-axis: labelled ticks at standard audio points + minor gridlines
from matplotlib.ticker import FixedLocator, NullFormatter
majf = [20,30,40,50,60,80,100,200,300,400,500,600,800,1000,2000,3000,4000,5000,6000,8000,10000,15000,20000]
labels=['%dk'%(f/1000) if f>=1000 else str(f) for f in majf]
minf=[i*10 for i in range(2,10)]+[i*100 for i in range(2,10)]+[i*1000 for i in range(2,20)]
for ax in (a1,a2):
    ax.set_xticks(majf); ax.set_xticklabels(labels, fontsize=7, rotation=45)
    ax.xaxis.set_minor_locator(FixedLocator(minf)); ax.xaxis.set_minor_formatter(NullFormatter())
    ax.grid(True, which='major', axis='x', alpha=0.35)
    ax.grid(True, which='minor', axis='x', alpha=0.12)
plt.tight_layout(); plt.savefig('speaker_response.png',dpi=110)
print('Saved plot: speaker_response.png')

# report gated levels + flag notches (candidate crossover suck-outs) in 300-6000 Hz
print('\nGated response (relative 500Hz-2kHz), candidate crossover band:')
for fc in [200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300]:
    j=np.argmin(np.abs(fb-fc)); print(f'{fc:6d} Hz  {m_gate[j]:6.1f} dB')
