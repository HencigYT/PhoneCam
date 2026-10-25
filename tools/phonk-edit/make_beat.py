#!/usr/bin/env python3
"""Synthesize a royalty-free drift-phonk style test track (cowbell melody, 808 slides,
punchy kick/clap, hats) with two bass drops, so the edit template can be tried without
any licensed music. Everything is generated from scratch, nothing is sampled.

    python make_beat.py phonk.wav --bpm 130 --bars 11
"""
import argparse
import wave

import numpy as np

SR = 44100


def note_hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12)


def place(buf, sig, t):
    i = int(t * SR)
    if i >= len(buf):
        return
    n = min(len(sig), len(buf) - i)
    buf[i:i + n] += sig[:n]


def env(n, decay, attack=0.002):
    t = np.arange(n) / SR
    a = np.clip(t / attack, 0, 1)
    return a * np.exp(-t / decay)


def kick(gain=1.0):
    n = int(0.45 * SR)
    t = np.arange(n) / SR
    f = 45 + 110 * np.exp(-t / 0.03)
    ph = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(ph) * env(n, 0.16)
    click = np.random.randn(n) * env(n, 0.004) * 0.3
    return np.tanh(2.2 * (body + click)) * gain


def bass808(f_from, f_to, length, gain=0.9):
    n = int(length * SR)
    t = np.arange(n) / SR
    glide = np.clip(t / 0.09, 0, 1)
    f = f_from + (f_to - f_from) * glide
    ph = 2 * np.pi * np.cumsum(f) / SR
    s = np.sin(ph) + 0.25 * np.sin(2 * ph)
    e = env(n, length * 0.55, 0.004)
    tail = np.clip((length - t) / 0.03, 0, 1)
    return np.tanh(3.0 * s * e) * tail * gain


def clap(gain=0.55):
    n = int(0.3 * SR)
    noise = np.random.randn(n)
    hp = np.diff(noise, prepend=0)
    e = np.zeros(n)
    for k, off in enumerate((0.0, 0.011, 0.022)):
        i = int(off * SR)
        e[i:] += env(n - i, 0.012 if k < 2 else 0.11) * (0.7 if k < 2 else 1.0)
    return np.tanh(1.5 * hp * e) * gain


def hat(gain=0.18, decay=0.035):
    n = int(0.12 * SR)
    noise = np.random.randn(n)
    hp = np.diff(np.diff(noise, prepend=0), prepend=0)
    return hp * env(n, decay) * gain * 0.5


def cowbell(midi, gain=0.32, length=0.28):
    n = int(length * SR)
    t = np.arange(n) / SR
    f1 = note_hz(midi)
    f2 = f1 * 1.48
    s = np.sign(np.sin(2 * np.pi * f1 * t)) + 0.8 * np.sign(np.sin(2 * np.pi * f2 * t))
    # crude band-pass: remove DC drift, then soften the top end with a one-pole lowpass
    s = np.diff(s, prepend=0) * 0.15 + s * 0.35
    out = np.empty_like(s)
    acc = 0.0
    for i, v in enumerate(s):
        acc += 0.35 * (v - acc)
        out[i] = acc
    return out * env(n, 0.09, 0.001) * gain


def riser(length, gain=0.25):
    n = int(length * SR)
    t = np.arange(n) / SR
    noise = np.diff(np.random.randn(n), prepend=0)
    return noise * (t / length) ** 2 * gain


def build(bpm, bars, seed):
    np.random.seed(seed)
    beat = 60.0 / bpm
    step = beat / 4
    bar = beat * 4
    total = bars * bar + 1.5
    mix = np.zeros(int(total * SR))
    drums = np.zeros_like(mix)

    # Structure: intro, drop, short break, second drop. Drops land on bar starts.
    drop1 = 3
    brk = 7
    drop2 = 8

    melody = [69, None, 72, 69, None, 76, 74, None, 69, None, 72, 74, 72, None, 67, None]
    bass_line = [(45, 45), (45, 45), (48, 43), (41, 45)]  # (midi from, midi to) per bar

    for b in range(bars):
        t0 = b * bar
        full = drop1 <= b < brk or b >= drop2
        intro = b < drop1
        for i, m in enumerate(melody):
            if m is not None:
                place(mix, cowbell(m, gain=0.22 if intro else 0.3), t0 + i * step)
        for i in range(16):
            vel = 1.0 if i % 2 == 0 else 0.6
            if full or (intro and b >= 1):
                place(drums, hat(0.16 * vel), t0 + i * step)
            if full and i in (6, 14):
                place(drums, hat(0.12, 0.012), t0 + i * step + step / 2)
        if full:
            for i in (0, 7, 10):
                place(drums, kick(), t0 + i * step)
            for i in (4, 12):
                place(drums, clap(), t0 + i * step)
            f_from, f_to = bass_line[b % 4]
            place(mix, bass808(note_hz(f_from + 12), note_hz(f_to), beat * 2.2), t0)
            place(mix, bass808(note_hz(f_to), note_hz(f_to), beat * 1.4), t0 + 2.5 * beat)
        if b == brk:
            place(mix, bass808(note_hz(45), note_hz(33), bar * 0.9, gain=0.8), t0)
        if b in (drop1 - 1, drop2 - 1):
            place(mix, riser(bar), t0)

    mix += drums
    mix = np.tanh(1.3 * mix)
    mix /= np.max(np.abs(mix)) + 1e-9
    return mix * 0.89, [drop1 * bar, drop2 * bar]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="output .wav path")
    ap.add_argument("--bpm", type=float, default=130)
    ap.add_argument("--bars", type=int, default=11)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    audio, drops = build(a.bpm, a.bars, a.seed)
    pcm = (np.clip(audio, -1, 1) * 32767).astype(np.int16)
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"wrote {a.out}: {len(audio) / SR:.1f}s, drops at " + ", ".join(f"{d:.2f}s" for d in drops))


if __name__ == "__main__":
    main()
