#!/usr/bin/env python3
"""Synthesize a deliberately 'AI-sounding' fixture for the naturalizer.

Characteristics chosen to trip DAAT's heuristics:
  - perfectly looped 4 s phrase (microRepetition)
  - brickwall-limited to ~5 dB crest (crestFactor)
  - dual mono, L/R correlation = 1.0 (stereoCorrelation, midSideRatio)
  - constant -72 dBFS noise floor (noiseFloorStationarity)
  - quantized, uniform arrangement (low spectralFlux variance)

Usage: python3 make_fixture.py [out.wav]
"""
import sys
import numpy as np

SR = 44100
BAR = 4.0            # 4 s loop
REPEATS = 6          # 24 s total
F0 = 220.0

def pluck(freq, t):
    # simple additive synth voice with exponential decay
    n = len(t)
    sig = np.zeros(n)
    for h, a in enumerate([1.0, 0.45, 0.22, 0.10], start=1):
        sig += a * np.sin(2 * np.pi * freq * h * t)
    env = np.exp(-t * 3.5)
    return sig * env

def main(out="ai_like.wav"):
    n_bar = int(BAR * SR)
    t = np.arange(n_bar) / SR
    bar = np.zeros(n_bar)

    # quantized 8th-note arpeggio, perfectly periodic
    scale = [0, 3, 7, 12, 15, 12, 7, 3]  # semitones, minor
    eighth = BAR / 8
    for i, st in enumerate(scale):
        freq = F0 * 2 ** (st / 12)
        start = int(i * eighth * SR)
        length = min(int(0.9 * eighth * SR), n_bar - start)
        bar[start:start + length] += pluck(freq, np.arange(length) / SR) * 0.5

    # simple bass pulse on quarters
    for i in range(4):
        start = int(i * (BAR / 4) * SR)
        length = min(int(0.8 * (BAR / 4) * SR), n_bar - start)
        bar[start:start + length] += pluck(F0 / 2, np.arange(length) / SR) * 0.6

    bar /= max(1e-9, np.abs(bar).max())

    # loop it exactly
    track = np.tile(bar, REPEATS)

    # brickwall limit to ~5 dB crest: hard clip then makeup gain
    track = np.tanh(track * 3.0)
    peak = np.abs(track).max()
    rms = np.sqrt(np.mean(track ** 2))
    crest_db = 20 * np.log10(peak / rms)
    print(f"pre-limit crest: {crest_db:.1f} dB")
    # drive harder until crest ~5 dB
    track = np.tanh(track * 6.0)
    track *= 0.89 / np.abs(track).max()

    # constant stationary noise floor at -72 dBFS
    rng = np.random.default_rng(7)
    track += rng.standard_normal(len(track)) * 10 ** (-72 / 20)

    # dual mono
    stereo = np.stack([track, track], axis=1)

    # 50 ms edge fades to avoid clicks
    fade = int(0.05 * SR)
    ramp = np.linspace(0, 1, fade)
    stereo[:fade] *= ramp[:, None]
    stereo[-fade:] *= ramp[::-1, None]

    # write 16-bit WAV
    pcm = np.clip(stereo, -1, 1)
    pcm16 = (pcm * 32767).astype(np.int16)
    import wave
    with wave.open(out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm16.tobytes())
    print(f"wrote {out}: {len(track)/SR:.1f} s, crest "
          f"{20*np.log10(np.abs(track).max()/np.sqrt(np.mean(track**2))):.1f} dB")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "ai_like.wav")
