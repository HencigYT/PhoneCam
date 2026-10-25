#!/usr/bin/env python3
"""Beat-synced phonk edit renderer.

Takes photos and/or video clips plus a music track and renders a vertical (9:16) edit:
glowing neon outline that tracks the subject, pulsing aura and particles, RGB split and
chromatic aberration, glitch transitions, white flashes, impact shakes, zoom/speed ramps
with motion blur and a dark cinematic grade. Cuts and effects are placed on the beats the
audio analysis finds, and the heaviest effects land on the detected bass drops.

Only use footage you own or have the subject's permission to edit and publish.

    python phonk_edit.py clip1.mp4 photo1.jpg photo2.jpg --audio track.mp3 -o edit.mp4
"""
import argparse
import math
import subprocess
import sys
import urllib.request
from pathlib import Path

import cv2
import librosa
import numpy as np

IMAGE_EXT = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}
VIDEO_EXT = {".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v"}
MODEL_URL = ("https://storage.googleapis.com/mediapipe-models/image_segmenter/"
             "selfie_multiclass_256x256/float32/latest/selfie_multiclass_256x256.tflite")
MODEL_PATH = Path.home() / ".cache" / "phonk-edit" / "selfie_multiclass_256x256.tflite"


# ---------------------------------------------------------------- segmentation

class Segmenter:
    """Person mask from MediaPipe's multiclass selfie model (hair, body, clothes)."""

    def __init__(self):
        import mediapipe as mp
        from mediapipe.tasks.python import BaseOptions, vision
        if not MODEL_PATH.exists():
            MODEL_PATH.parent.mkdir(parents=True, exist_ok=True)
            print(f"downloading segmentation model to {MODEL_PATH}", file=sys.stderr)
            urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
        self.mp = mp
        opts = vision.ImageSegmenterOptions(
            base_options=BaseOptions(model_asset_path=str(MODEL_PATH)),
            output_confidence_masks=True, output_category_mask=False)
        self.seg = vision.ImageSegmenter.create_from_options(opts)

    def __call__(self, bgr):
        h, w = bgr.shape[:2]
        k = 640 / max(h, w)
        small = cv2.resize(bgr, (round(w * k), round(h * k)), interpolation=cv2.INTER_AREA) if k < 1 else bgr
        rgb = np.ascontiguousarray(cv2.cvtColor(small, cv2.COLOR_BGR2RGB))
        res = self.seg.segment(self.mp.Image(image_format=self.mp.ImageFormat.SRGB, data=rgb))
        person = 1.0 - res.confidence_masks[0].numpy_view().astype(np.float32)
        return cv2.resize(person, (w, h), interpolation=cv2.INTER_LINEAR)

    def close(self):
        self.seg.close()


def main_subject(mask):
    """Keep the most prominent person (large and near the centre), drop people at the edges."""
    h, w = mask.shape
    binary = (mask > 0.5).astype(np.uint8)
    n, labels, stats, cents = cv2.connectedComponentsWithStats(binary, 8)
    if n <= 1:
        return mask
    best, best_score = 1, -1.0
    for i in range(1, n):
        dx = abs(cents[i][0] / w - 0.5)
        score = stats[i, cv2.CC_STAT_AREA] * (1.0 - 1.4 * dx)
        if score > best_score:
            best, best_score = i, score
    keep = (labels == best).astype(np.uint8)
    keep = cv2.dilate(keep, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (15, 15)))
    return mask * keep


def mask_points(mask):
    """Horizontal subject centre and a face/upper-body point to zoom towards."""
    h, w = mask.shape
    ys, xs = np.nonzero(mask > 0.5)
    if len(xs) < 50:
        return (w / 2, h / 2), (w / 2, h * 0.4)
    top, bottom = ys.min(), ys.max()
    upper = ys < top + 0.3 * (bottom - top)
    return (xs.mean(), h / 2), (xs[upper].mean(), ys[upper].mean())


# ---------------------------------------------------------------- sources

class Still:
    def __init__(self, path, segmenter, mask_dir):
        self.img = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if self.img is None:
            raise SystemExit(f"cannot read image {path}")
        manual = Path(mask_dir) / (path.stem + ".png") if mask_dir else None
        if manual and manual.exists():
            m = cv2.imread(str(manual), cv2.IMREAD_GRAYSCALE).astype(np.float32) / 255
            self.mask = cv2.resize(m, self.img.shape[1::-1])
        elif segmenter:
            self.mask = main_subject(segmenter(self.img))
        else:
            self.mask = np.zeros(self.img.shape[:2], np.float32)
        self.center, self.focus = mask_points(self.mask)
        self.size = self.img.shape[1::-1]

    def start(self, speed_fn, n_frames, fps):
        pass

    def frame(self, i):
        return self.img, self.mask


class Clip:
    """Video source with time remapping (speed ramps) and temporally smoothed masks."""

    def __init__(self, path, segmenter):
        self.path = path
        self.cap = cv2.VideoCapture(str(path))
        if not self.cap.isOpened():
            raise SystemExit(f"cannot open video {path}")
        self.fps = self.cap.get(cv2.CAP_PROP_FPS) or 30
        self.count = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT)) or 1
        self.seg = segmenter
        self.cursor = 0.0  # source seconds already used, so repeats show new footage
        ok, first = self.cap.read()
        if not ok:
            raise SystemExit(f"empty video {path}")
        m = main_subject(segmenter(first)) if segmenter else np.zeros(first.shape[:2], np.float32)
        self.center, self.focus = mask_points(m)
        self.size = first.shape[1::-1]

    def start(self, speed_fn, n_frames, fps):
        dur = self.count / self.fps
        times = np.cumsum([speed_fn(k / max(n_frames - 1, 1)) / fps for k in range(n_frames)])
        if self.cursor + times[-1] > dur:
            self.cursor = 0.0
        self.times = np.minimum(self.cursor + times, dur - 1 / self.fps)
        self.cursor = float(self.times[-1])
        self.cap.set(cv2.CAP_PROP_POS_FRAMES, int(self.times[0] * self.fps))
        self.pos = int(self.times[0] * self.fps)
        self.last = None
        self.ema = None

    def frame(self, i):
        want = int(self.times[i] * self.fps)
        while self.last is None or self.pos <= want:
            ok, img = self.cap.read()
            if not ok:
                break
            self.last = img
            self.pos += 1
        m = main_subject(self.seg(self.last)) if self.seg else np.zeros(self.last.shape[:2], np.float32)
        self.ema = m if self.ema is None else 0.55 * m + 0.45 * self.ema
        return self.last, self.ema


# ---------------------------------------------------------------- audio analysis

def impulses(times, weights, t, tau):
    """Sum of exponentially decaying impulses sampled at frame times t."""
    out = np.zeros_like(t)
    for h, w in zip(times, weights):
        d = t - h
        on = d >= 0
        out[on] += w * np.exp(-d[on] / tau)
    return np.clip(out, 0, 1.5)


def analyse(audio, start, duration, fps, drops_override, bpm):
    y, sr = librosa.load(audio, sr=22050, mono=True, offset=start, duration=duration)
    dur = len(y) / sr
    hop = 256
    # Phonk sits around 120-160 BPM; without a prior, short clips often lock onto 2/3 tempo.
    opts = dict(bpm=bpm) if bpm else dict(start_bpm=130)
    tempo, beat_frames = librosa.beat.beat_track(y=y, sr=sr, hop_length=hop, **opts)
    beats = librosa.frames_to_time(beat_frames, sr=sr, hop_length=hop)
    if len(beats) < 4:
        step = 60 / (bpm or 130)
        beats = np.arange(0, dur, step)

    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=hop))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
    ft = librosa.frames_to_time(np.arange(S.shape[1]), sr=sr, hop_length=hop)
    low = S[freqs < 150].sum(0)
    low /= np.percentile(low, 98) + 1e-9
    mel = librosa.feature.melspectrogram(S=S ** 2, sr=sr)
    mel_f = librosa.mel_frequencies(n_mels=mel.shape[0], fmax=sr / 2)
    lo_on = librosa.onset.onset_strength(S=librosa.power_to_db(mel[mel_f < 200]), sr=sr, hop_length=hop)
    hi_on = librosa.onset.onset_strength(S=librosa.power_to_db(mel[mel_f > 2000]), sr=sr, hop_length=hop)

    def peaks(env, pct):
        idx = librosa.onset.onset_detect(onset_envelope=env, sr=sr, hop_length=hop, units="frames")
        if len(idx) == 0:
            return np.array([]), np.array([])
        strength = env[idx] / (env.max() + 1e-9)
        keep = strength >= np.percentile(strength, pct)
        return librosa.frames_to_time(idx[keep], sr=sr, hop_length=hop), strength[keep] / strength[keep].max()

    kicks, kick_w = peaks(lo_on, 45)
    snares, snare_w = peaks(hi_on, 55)

    if drops_override:
        drops = np.array(sorted(drops_override)) - start
    else:
        # A drop is where the bass energy jumps: compare the next ~1 s with the previous ~2 s.
        rate = 1 / (ft[1] - ft[0])
        ahead = int(1.0 * rate)
        behind = int(2.0 * rate)
        score = np.zeros_like(low)
        for i in range(behind, len(low) - ahead):
            score[i] = low[i:i + ahead].mean() - low[i - behind:i].mean()
        drops = []
        for i in np.argsort(-score):
            t = ft[i]
            if score[i] < 0.18 or len(drops) >= 3:
                break
            if 1.0 < t < dur - 2.0 and all(abs(t - d) > 4.0 for d in drops):
                drops.append(t)
        if not drops:
            drops = [ft[int(np.argmax(score))]]
        drops = np.array(sorted(drops))
    drops = np.array([beats[np.argmin(np.abs(beats - d))] if np.min(np.abs(beats - d)) < 0.2 else d
                      for d in drops])

    t = np.arange(int(dur * fps)) / fps
    bass = np.interp(t, ft, np.convolve(low, np.ones(5) / 5, mode="same"))
    return dict(duration=dur, tempo=float(np.atleast_1d(tempo)[0]), beats=beats, drops=drops,
                kicks=kicks, kick_w=kick_w, snares=snares, snare_w=snare_w, t=t, bass=np.clip(bass, 0, 1.2))


# ---------------------------------------------------------------- timeline

def plan_cuts(beats, drops, dur):
    """Long cuts in the intro, two-beat cuts after a drop, one-beat cuts in the bar right after it."""
    beat_len = float(np.median(np.diff(beats))) if len(beats) > 1 else 0.46
    first_drop = drops[0] if len(drops) else dur
    points = sorted(set(np.round(np.concatenate([beats, drops]), 4)))
    cuts, last, count = [0.0], 0.0, 0
    for p in points:
        if p <= 0.05 or p >= dur - 0.15:
            continue
        count += 1
        is_drop = any(abs(p - d) < 1e-3 for d in drops)
        since_drop = min([p - d for d in drops if d <= p + 1e-3], default=None)
        if p < first_drop - 1e-3:
            need = 4
        elif since_drop is not None and since_drop < 4 * beat_len - 1e-3:
            need = 1
        else:
            need = 2
        if is_drop or count >= need:
            if p - last > 0.15:
                cuts.append(p)
                last = p
            count = 0
    cuts.append(dur)
    return cuts


def ease_out(x):
    return 1 - (1 - x) ** 3


def zoom_curve(kind, seg_len):
    """Zoom factor and playback speed as functions of the segment position u in [0, 1]."""
    if kind == "push":  # intro: slow, smooth push in
        return (lambda u: 1.04 + 0.16 * (u * u * (3 - 2 * u)),
                lambda u: 0.75)
    if kind == "drop":  # big punch: start very close, snap out, slow motion afterwards
        return (lambda u: 1.06 + 0.7 * math.exp(-u * seg_len / 0.12) + 0.06 * u,
                lambda u: 2.4 if u * seg_len < 0.15 else 0.45)
    if kind == "punch":
        return (lambda u: 1.05 + 0.3 * math.exp(-u * seg_len / 0.09) + 0.04 * u,
                lambda u: 2.0 if u * seg_len < 0.1 else 0.6)
    # "ramp": hold, then accelerate into the next cut (velocity zoom)
    return (lambda u: 1.05 + 0.05 * u + 0.55 * u ** 6,
            lambda u: 0.5 + 2.8 * u ** 4)


# ---------------------------------------------------------------- effects

def hex_bgr(s):
    s = s.strip().lstrip("#")
    r, g, b = (int(s[i:i + 2], 16) for i in (0, 2, 4))
    return np.array([b, g, r], np.float32) / 255


def screen(a, b):
    return 1 - (1 - a) * (1 - np.clip(b, 0, 1))


def grade(img, mask, strength):
    lum = img @ np.array([0.114, 0.587, 0.299], np.float32)
    m = mask[..., None]
    # background: crushed, desaturated, cold
    bg = (0.7 * lum[..., None] + 0.3 * img) * 0.33 * np.array([1.18, 1.02, 0.82], np.float32)
    # subject: filmic contrast, teal shadows, warm highlights, black point lift removed
    s = img + 0.55 * (img * img * (3 - 2 * img) - img)
    l = s @ np.array([0.114, 0.587, 0.299], np.float32)
    s = s + (1 - l[..., None]) * np.array([0.03, 0.01, -0.02], np.float32) \
          + l[..., None] * np.array([-0.035, 0.0, 0.03], np.float32)
    s = np.clip((s - 0.025) / 0.975, 0, 1)
    out = bg * (1 - m) + s * m
    return img + strength * (out - img)


class Particles:
    def __init__(self, rng, scale):
        self.rng = rng
        self.k = scale
        self.p = np.zeros((0, 7), np.float32)  # x, y, vx, vy, life, max_life, size

    def step(self, edge_pts, spawn, dt):
        if len(edge_pts) and spawn > 0:
            idx = self.rng.integers(0, len(edge_pts), int(spawn))
            pts = edge_pts[idx].astype(np.float32)
            n = len(pts)
            new = np.column_stack([
                pts[:, 1], pts[:, 0],
                self.rng.normal(0, 25, n) * self.k, -self.rng.uniform(40, 140, n) * self.k,
                np.zeros(n), self.rng.uniform(0.5, 1.4, n), self.rng.uniform(1.0, 2.6, n) * self.k])
            self.p = np.vstack([self.p, new])[-450:]
        p = self.p
        p[:, 4] += dt
        p[:, 2] += np.sin(p[:, 1] * 0.02 + p[:, 4] * 3) * 30 * self.k * dt
        p[:, 0] += p[:, 2] * dt
        p[:, 1] += p[:, 3] * dt
        self.p = p[p[:, 4] < p[:, 5]]

    def draw(self, shape):
        layer = np.zeros(shape, np.float32)
        for x, y, _, _, life, mx, size in self.p:
            a = math.sin(math.pi * life / mx)
            cv2.circle(layer, (int(x), int(y)), max(1, int(size)), float(a), -1, cv2.LINE_AA)
        return layer


class Renderer:
    def __init__(self, W, H, colors, rng):
        self.W, self.H = W, H
        self.k = W / 1080
        self.colors = colors
        self.rng = rng
        self.particles = Particles(rng, self.k / 2)
        yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
        r = np.sqrt(((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2) / math.sqrt(2)
        self.vignette = (1 - 0.62 * r ** 2.2)[..., None].astype(np.float32)
        self.grain = [rng.normal(0, 1, (H // 2, W // 2)).astype(np.float32) for _ in range(6)]
        self.ygrad = np.linspace(0, 1, H // 2, dtype=np.float32)[:, None]
        self.prev = None

    def neon(self, mask, t, bass, hit, intensity):
        """Outline + glow + aura + particles, computed at half resolution."""
        w, h = self.W // 2, self.H // 2
        m = cv2.resize(mask, (w, h), interpolation=cv2.INTER_AREA)
        b = (m > 0.5).astype(np.uint8)
        if b.sum() < 30:
            return None
        k = max(3, int(5 * self.k) | 1)
        edge = cv2.morphologyEx(b, cv2.MORPH_GRADIENT, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k)))
        edge = cv2.GaussianBlur(edge.astype(np.float32), (0, 0), 0.8)
        xs = np.arange(w, dtype=np.float32)[None, :]
        shimmer = 0.78 + 0.22 * np.sin((xs * 0.9 + self.ygrad * h * 0.6) * 0.06 - t * 9)
        core = edge * shimmer
        g1 = cv2.GaussianBlur(edge, (0, 0), 4 * self.k)
        g2 = cv2.GaussianBlur(edge, (0, 0), 13 * self.k)
        q = cv2.resize(b.astype(np.float32), (w // 2, h // 2), interpolation=cv2.INTER_AREA)
        aura = cv2.resize(cv2.GaussianBlur(q, (0, 0), 16 * self.k), (w, h)) * (1 - m)
        pulse = 0.55 + 0.6 * bass + 0.5 * hit
        line = (core * 1.25 + g1 * 1.7 + g2 * 2.4 * pulse) * (0.55 + 0.45 * intensity)
        mix = 0.5 + 0.5 * np.sin(2 * math.pi * (self.ygrad * 0.9 - t * 0.35))
        c1, c2 = self.colors
        color = c1 * mix[..., None] + c2 * (1 - mix[..., None])  # (h,1,3)
        out = line[..., None] * color + aura[..., None] * c1 * 0.5 * pulse * intensity

        pts = np.argwhere(edge > 0.5)
        self.particles.step(pts, (2 + 26 * hit) * intensity, 1 / 30)
        dots = self.particles.draw((h, w))
        dots = dots + cv2.GaussianBlur(dots, (0, 0), 3 * self.k) * 2
        out += dots[..., None] * (0.6 * c2 + 0.4)
        return cv2.resize(out, (self.W, self.H), interpolation=cv2.INTER_LINEAR)

    def rgb_split(self, img, amount, radial):
        b, g, r = cv2.split(img)
        cx, cy = self.W / 2, self.H / 2

        def warp(ch, s, dx):
            M = np.float32([[s, 0, (1 - s) * cx + dx], [0, s, (1 - s) * cy]])
            return cv2.warpAffine(ch, M, (self.W, self.H), borderMode=cv2.BORDER_REFLECT)

        r = warp(r, 1 + radial, amount)
        b = warp(b, 1 - radial, -amount)
        return cv2.merge([b, g, r])

    def glitch(self, img, g, seed):
        rng = np.random.default_rng(seed)
        out = img.copy()
        H, W = self.H, self.W
        for _ in range(int(5 + 14 * g)):
            y = int(rng.integers(0, H))
            h = int(rng.integers(int(6 * self.k) + 1, int(90 * self.k) + 2))
            dx = int(rng.normal(0, 120 * g * self.k))
            band = out[y:y + h]
            if rng.random() < 0.35:
                c = int(rng.integers(0, 3))
                band[..., c] = np.roll(band[..., c], dx * 2, axis=1)
            else:
                out[y:y + h] = np.roll(band, dx, axis=1)
        for _ in range(int(2 + 5 * g)):
            bw, bh = int(rng.integers(40, 260) * self.k), int(rng.integers(10, 80) * self.k)
            x, y = int(rng.integers(0, W - bw)), int(rng.integers(0, H - bh))
            sx = int(np.clip(x + rng.normal(0, 150 * self.k), 0, W - bw))
            out[y:y + bh, x:x + bw] = out[y:y + bh, sx:sx + bw][..., ::-1] if rng.random() < 0.3 \
                else out[y:y + bh, sx:sx + bw]
        out[::3] *= 1 - 0.18 * g
        return out

    def zoom_blur(self, img, focus, amount):
        out = img.copy()
        n = 4
        for i in range(1, n):
            s = 1 + amount * i / n
            M = np.float32([[s, 0, (1 - s) * focus[0]], [0, s, (1 - s) * focus[1]]])
            out += cv2.warpAffine(img, M, (self.W, self.H), borderMode=cv2.BORDER_REFLECT)
        return out / n


# ---------------------------------------------------------------- main loop

def view_matrix(src, z, shake, rot, W, H):
    sw, sh = src.size
    cover = max(W / sw, H / sh)
    s = cover * z
    k = min(1.0, max(0.0, (z - 1.04) / 0.5)) * 0.85
    cx = src.center[0] + (src.focus[0] - src.center[0]) * k
    cy = src.center[1] + (src.focus[1] - src.center[1]) * k
    hw, hh = W / 2 / s, H / 2 / s
    cx = min(max(cx, hw), sw - hw)
    cy = min(max(cy, hh), sh - hh)
    a = math.radians(rot)
    ca, sa = math.cos(a) * s, math.sin(a) * s
    tx = W / 2 + shake[0] - (ca * cx - sa * cy)
    ty = H / 2 + shake[1] - (sa * cx + ca * cy)
    M = np.float32([[ca, -sa, tx], [sa, ca, ty]])
    fx, fy = src.focus
    focus_out = (ca * fx - sa * fy + tx, sa * fx + ca * fy + ty)
    return M, focus_out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", type=Path, help="photos and/or video clips, in order")
    ap.add_argument("--audio", required=True, help="music track (licensed, royalty-free or your own)")
    ap.add_argument("-o", "--out", default="phonk_edit.mp4")
    ap.add_argument("--start", type=float, default=0.0, help="start offset in the track (s)")
    ap.add_argument("--duration", type=float, default=20.0, help="edit length (s), 15-25 recommended")
    ap.add_argument("--drops", type=lambda s: [float(x) for x in s.split(",")],
                    help="override detected drop times, seconds in the track, e.g. 5.5,14.8")
    ap.add_argument("--bpm", type=float, help="exact track tempo, if beat detection is off")
    ap.add_argument("--hero", type=int, default=0, help="input index shown on each drop")
    ap.add_argument("--neon", default="#c13bff,#2ee6ff", help="two outline colours, hex")
    ap.add_argument("--grade", type=float, default=1.0, help="colour grade strength 0-1")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--scale", type=float, default=1.0, help="0.5 renders a fast 540x960 preview")
    ap.add_argument("--mask-dir", help="optional folder of <photo-name>.png masks (white = subject)")
    ap.add_argument("--no-seg", action="store_true", help="skip automatic segmentation")
    ap.add_argument("--crf", type=int, default=17)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    W, H = int(1080 * a.scale) // 2 * 2, int(1920 * a.scale) // 2 * 2
    fps = a.fps
    rng = np.random.default_rng(a.seed)
    colors = [hex_bgr(c) for c in a.neon.split(",")[:2]]
    if len(colors) == 1:
        colors *= 2

    info = analyse(a.audio, a.start, a.duration, fps, a.drops, a.bpm)
    dur, t = info["duration"], info["t"]
    drops = info["drops"]
    print(f"tempo {info['tempo']:.1f} BPM, {len(info['beats'])} beats, drops at "
          + ", ".join(f"{d + a.start:.2f}s" for d in drops), file=sys.stderr)

    segmenter = None if a.no_seg else Segmenter()
    sources = []
    for p in a.inputs:
        ext = p.suffix.lower()
        if ext in IMAGE_EXT:
            sources.append(Still(p, segmenter, a.mask_dir))
        elif ext in VIDEO_EXT:
            sources.append(Clip(p, segmenter))
        else:
            raise SystemExit(f"unsupported input {p}")

    # Envelopes per output frame.
    first_drop = drops[0] if len(drops) else dur
    intensity = np.where(t < first_drop, 0.45, 1.0)
    kick = impulses(info["kicks"], info["kick_w"], t, 0.13) * intensity
    snare = impulses(info["snares"], info["snare_w"], t, 0.08) * intensity
    drop_imp = impulses(drops, np.ones(len(drops)), t, 0.45)
    cuts = plan_cuts(info["beats"], drops, dur)
    flash_t, flash_w = list(drops), [1.0] * len(drops)
    glitch = np.zeros_like(t)
    for d in drops:
        i = int(d * fps)
        glitch[i:i + 8] = np.maximum(glitch[i:i + 8], np.linspace(1.0, 0.4, len(glitch[i:i + 8])))
        j = int((d - 0.25) * fps)  # short stutter leading into the drop
        glitch[max(j, 0):max(j, 0) + 3] = 0.5
    for n, c in enumerate(cuts[1:-1]):
        if c >= first_drop and not any(abs(c - d) < 1e-3 for d in drops):
            i = int(c * fps)
            glitch[i:i + 2] = np.maximum(glitch[i:i + 2], 0.55)
            if n % 4 == 0:
                flash_t.append(c)
                flash_w.append(0.55)
    for s, w in zip(info["snares"], info["snare_w"]):
        if s >= first_drop and rng.random() < 0.25 * w:
            i = int(s * fps)
            glitch[i:i + 2] = np.maximum(glitch[i:i + 2], 0.3)
    flash = impulses(flash_t, flash_w, t, 0.07)

    # Segments.
    segs = []
    for n, (c0, c1) in enumerate(zip(cuts[:-1], cuts[1:])):
        f0, f1 = int(round(c0 * fps)), int(round(c1 * fps))
        if f1 <= f0:
            continue
        is_drop = any(abs(c0 - d) < 1e-3 for d in drops)
        if c0 < first_drop - 1e-3:
            kind = "push"
        elif is_drop:
            kind = "drop"
        else:
            kind = "punch" if n % 2 else "ramp"
        idx = a.hero % len(sources) if is_drop else n % len(sources)
        segs.append((f0, f1, kind, idx))

    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgr24",
           "-s", f"{W}x{H}", "-r", str(fps), "-i", "-",
           "-ss", str(a.start), "-t", f"{dur:.3f}", "-i", a.audio,
           "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-preset", "slow", "-crf", str(a.crf),
           "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k",
           "-af", f"afade=t=out:st={max(dur - 0.8, 0):.3f}:d=0.8", "-shortest",
           "-movflags", "+faststart", a.out]
    enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    R = Renderer(W, H, colors, rng)
    total = len(t)

    for f0, f1, kind, idx in segs:
        src = sources[idx]
        n = f1 - f0
        seg_len = n / fps
        zf, sp = zoom_curve(kind, seg_len)
        src.start(sp, n, fps)
        zs = [zf(k / max(n - 1, 1)) for k in range(n + 1)]
        for k in range(n):
            fi = f0 + k
            if fi >= total:
                break
            tt = t[fi]
            amp = (34 * kick[fi] + 70 * drop_imp[fi]) * R.k
            shake = rng.normal(0, 1, 2) * amp
            rot = rng.normal(0, 1) * (0.9 * kick[fi] + 1.8 * drop_imp[fi])
            img, mask = src.frame(k)
            M, focus = view_matrix(src, zs[k], shake, rot, W, H)
            frame = cv2.warpAffine(img, M, (W, H), flags=cv2.INTER_LINEAR,
                                   borderMode=cv2.BORDER_REFLECT).astype(np.float32) / 255
            m = cv2.warpAffine(mask, M, (W, H), flags=cv2.INTER_LINEAR)

            out = grade(frame, m, a.grade)
            glow = R.neon(m, tt, info["bass"][fi], kick[fi] + drop_imp[fi], intensity[fi])
            if glow is not None:
                out = screen(out, glow)

            vel = abs(zs[k + 1] - zs[k]) * fps
            if vel > 0.6:
                out = R.zoom_blur(out, focus, min(0.12, vel * 0.012))
            if R.prev is not None and (kind == "ramp" and k > 0.7 * n):
                out = 0.7 * out + 0.3 * R.prev  # smear on the velocity ramp
            split = (3 + 22 * kick[fi] + 30 * drop_imp[fi] + 16 * glitch[fi]) * R.k
            out = R.rgb_split(out, split, 0.002 + 0.01 * (kick[fi] + drop_imp[fi]))
            if glitch[fi] > 0:
                out = R.glitch(out, glitch[fi], a.seed * 100003 + fi)
            out = out * R.vignette
            g = cv2.resize(R.grain[fi % len(R.grain)], (W, H))
            out = out + g[..., None] * 0.018
            out = screen(out, np.full_like(out[..., :1], min(flash[fi], 1.0)))
            if tt > dur - 0.6:
                out *= max(0.0, (dur - tt) / 0.6)
            R.prev = out
            enc.stdin.write((np.clip(out, 0, 1) * 255).astype(np.uint8).tobytes())
            if fi % fps == 0:
                print(f"\rrendering {fi / fps:5.1f}/{dur:.1f}s", end="", file=sys.stderr)

    enc.stdin.close()
    enc.wait()
    if segmenter:
        segmenter.close()
    print(f"\nwrote {a.out} ({W}x{H}, {dur:.1f}s)", file=sys.stderr)
    if enc.returncode:
        raise SystemExit("ffmpeg failed")


if __name__ == "__main__":
    main()
