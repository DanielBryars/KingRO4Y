"""Log-sine-sweep (Farina) measurement of one speaker: frequency response, phase, polarity, delay."""
import numpy as np, sounddevice as sd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from audio_lib import find_device, play_record, dbfs, FS

fs = FS
MIC_CH = 0                      # Scarlett input 1
f1, f2 = 20.0, 20000.0
T = 6.0                         # sweep length (s)
amp = 10**(-12/20)             # -12 dBFS

in_dev, ind, in_api   = find_device('Scarlett', 'input')
out_dev, outd, out_api = find_device('Digital Output (Realtek', 'output')
out_ch = min(2, outd['max_output_channels'])
print(f'INPUT : [{in_dev}] {ind["name"]} ({in_api}) using ch{MIC_CH+1}')
print(f'OUTPUT: [{out_dev}] {outd["name"]} ({out_api}) LEFT of {out_ch}ch')

# ---- Farina log sweep ----
N = int(T*fs)
t = np.arange(N)/fs
K = T*2*np.pi*f1/np.log(f2/f1)
L = np.log(f2/f1)
x = np.sin(K*(np.exp(t/T*L)-1.0)).astype('float32')
# fades
fd = int(0.02*fs); env = np.ones(N); env[:fd]=np.hanning(2*fd)[:fd]; env[-fd:]=np.hanning(2*fd)[fd:]
x *= env
# inverse filter (amplitude-corrected, time-reversed)
inv = x[::-1] * np.exp(-t/T*L)
inv /= np.max(np.abs(inv))

# output: sweep on LEFT, pre/post silence
pre, post = int(0.3*fs), int(0.5*fs)
sig = np.zeros((pre+N+post, out_ch), dtype='float32')
sig[pre:pre+N, 0] = amp*x

print('Playing sweep...')
rec = play_record(sig, in_dev, out_dev, in_channels=max(2, ind['max_input_channels']), fs=fs)
y = rec[:, MIC_CH].astype('float64')
print(f'Mic capture: peak {20*np.log10(np.max(np.abs(y))+1e-12):.1f} dBFS, RMS {dbfs(y):.1f} dBFS')

# ---- deconvolve -> impulse response ----
Lc = len(y)+len(inv)-1; nfft = 1<<int(np.ceil(np.log2(Lc)))
ir = np.fft.irfft(np.fft.rfft(y,nfft)*np.fft.rfft(inv,nfft), nfft)
pk = int(np.argmax(np.abs(ir)))
polarity = '+ (normal)' if ir[pk] > 0 else '- (INVERTED)'
# delay: the linear part of the sweep deconvolution centers the direct sound at the inverse-filter length
delay_samp = pk - (len(inv)-1)
delay_ms = delay_samp/fs*1e3
dist_m = delay_ms/1000*343.0
print(f'IR peak sign -> polarity {polarity}')
print(f'Direct-sound delay {delay_ms:.2f} ms  ->  mic distance ~{dist_m*100:.0f} cm')

# window IR around the peak for the transfer function (include some room, exclude late reflections lightly)
w0 = pk - int(0.005*fs)
w1 = pk + int(0.05*fs)          # 50 ms window
w0 = max(0,w0)
irw = ir[w0:w1] * np.hanning(w1-w0)
NF = 1<<16
H = np.fft.rfft(irw, NF)
freq = np.fft.rfftfreq(NF, 1/fs)
mag = 20*np.log10(np.abs(H)+1e-12)

# fractional-octave smoothing (1/6 oct)
def smooth(freq, mag, frac=6):
    out = np.copy(mag)
    for i,f in enumerate(freq):
        if f<=0: continue
        lo,hi = f/2**(1/(2*frac)), f*2**(1/(2*frac))
        m = (freq>=lo)&(freq<=hi)
        if m.any(): out[i]=np.mean(mag[m])
    return out
band = (freq>=15)&(freq<=22000)
mags = smooth(freq[band], mag[band])
fb = freq[band]
# normalize to mean 200 Hz - 2 kHz
ref = (fb>=200)&(fb<=2000)
mags -= np.mean(mags[ref])

# phase, delay removed, over band
phase = np.unwrap(np.angle(H))
# remove bulk delay (pk) so phase reflects the speaker, not propagation
phase_corr = np.angle(H*np.exp(1j*2*np.pi*freq*(pk)/fs))
phb = np.unwrap(phase_corr[band])

# ---- text summary: 1/1-octave band levels ----
print('\nFrequency response (relative to 200Hz-2kHz mean), 1/6-oct smoothed:')
for fc in [20,25,31.5,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000]:
    j = np.argmin(np.abs(fb-fc))
    v = mags[j]
    bar = '#'*int(max(0,min(40, 20+v)))
    print(f'{fc:7.0f} Hz  {v:6.1f} dB  {bar}')

# -3/-6 dB bandwidth estimate
passmean = 0.0
def find_corner(fb,mags,side):
    idx = np.where((fb>=40)&(fb<=20000))[0]
    for j in (idx if side=='low' else idx[::-1]):
        if mags[j] >= -6:
            return fb[j]
    return None
print(f'\nApprox -6 dB band edges: low ~{find_corner(fb,mags,"low"):.0f} Hz, high ~{find_corner(fb,mags,"high"):.0f} Hz')

# ---- plot ----
fig,(ax1,ax2)=plt.subplots(2,1,figsize=(10,7),sharex=True)
ax1.semilogx(fb,mags,color='tab:blue')
ax1.set_ylabel('Magnitude (dB)'); ax1.set_title('Speaker frequency response (1/6-oct, log sweep)')
ax1.grid(True,which='both',alpha=0.3); ax1.set_ylim(-30,15)
for fc in [20,50,100,200,500,1000,2000,5000,10000,20000]: ax1.axvline(fc,color='k',alpha=0.05)
ax2.semilogx(fb,np.degrees(phb),color='tab:red')
ax2.set_ylabel('Phase (deg, delay-removed)'); ax2.set_xlabel('Frequency (Hz)')
ax2.grid(True,which='both',alpha=0.3); ax2.set_xlim(20,22000)
out_png = 'speaker_response.png'
plt.tight_layout(); plt.savefig(out_png,dpi=110)
print(f'\nSaved plot: {out_png}')
np.save('ir.npy', ir)
