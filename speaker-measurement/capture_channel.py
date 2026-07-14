"""Capture one speaker via log sweep on a chosen output channel; save its impulse response.
Usage: python capture_channel.py L|R  out.npy
Mic gain and sweep level are identical between runs, so the saved IRs are level-comparable."""
import sys, numpy as np
from audio_lib import find_device, play_record, dbfs, FS

chan = (sys.argv[1] if len(sys.argv) > 1 else 'L').upper()
outfile = sys.argv[2] if len(sys.argv) > 2 else 'ir.npy'
ch_idx = 0 if chan == 'L' else 1
fs = FS; MIC_CH = 0
f1, f2, T = 20.0, 20000.0, 6.0
amp = 10**(-12/20)

in_dev, ind, _ = find_device('Scarlett', 'input')
out_dev, outd, _ = find_device('Digital Output (Realtek', 'output')
out_ch = min(2, outd['max_output_channels'])
print(f'Measuring {chan} channel (idx {ch_idx}) -> {outfile}')

N = int(T*fs); t = np.arange(N)/fs
K = T*2*np.pi*f1/np.log(f2/f1); Ln = np.log(f2/f1)
x = np.sin(K*(np.exp(t/T*Ln)-1.0)).astype('float32')
fd = int(0.02*fs); env = np.ones(N); env[:fd]=np.hanning(2*fd)[:fd]; env[-fd:]=np.hanning(2*fd)[fd:]
x *= env
inv = x[::-1]*np.exp(-t/T*Ln); inv /= np.max(np.abs(inv))

pre, post = int(0.3*fs), int(0.5*fs)
sig = np.zeros((pre+N+post, out_ch), dtype='float32')
sig[pre:pre+N, ch_idx] = amp*x

rec = play_record(sig, in_dev, out_dev, in_channels=max(2, ind['max_input_channels']), fs=fs)
y = rec[:, MIC_CH].astype('float64')
print(f'Mic capture: peak {20*np.log10(np.max(np.abs(y))+1e-12):.1f} dBFS, RMS {dbfs(y):.1f} dBFS')

nfft = 1<<int(np.ceil(np.log2(len(y)+len(inv)-1)))
ir = np.fft.irfft(np.fft.rfft(y,nfft)*np.fft.rfft(inv,nfft), nfft)
pk = int(np.argmax(np.abs(ir)))
print(f'polarity {"+ (normal)" if ir[pk]>0 else "- (INVERTED)"}, IR peak |{abs(ir[pk]):.4g}|')
np.save(outfile, ir)
print(f'saved {outfile}')
