# Phonk edit template

**[Magyar](#magyar) · [English](#english)**

Renders a vertical 1080x1920 beat-synced phonk edit from photos and/or video clips plus a music
track. Only use footage you own or that the people in it have agreed to be edited and published.

## English

### What it does

- **Neon outline** around the main subject (MediaPipe person segmentation, hair included),
  two-colour animated gradient, shimmer, glow that pulses with the bass
- **Aura and particles** rising from the silhouette, spawning faster on kicks
- **RGB split / chromatic aberration** that spikes on 808 and kick hits
- **Glitch** slices and block corruption on drops, cuts and some snares, plus a stutter just before each drop
- **White flashes** on drops and every few cuts, **impact shakes** with a little rotation on kicks
- **Speed ramps / velocity**: punch-in zooms, slow push-ins, accelerating zooms into the next cut
  with zoom blur; video clips are time-remapped (fast, then slow motion)
- **Cinematic grade**: crushed, desaturated, cold background, contrasty subject with teal shadows
  and warm highlights, vignette, film grain, fade out at the end
- **Beat sync**: beats, kicks, snares and bass drops come from audio analysis. Long cuts in the
  intro, 1-beat cuts right after a drop, 2-beat cuts afterwards. Heaviest effects land on the drops.

### Setup

```sh
pip install -r requirements.txt
# Debian/Ubuntu also needs ffmpeg and the EGL runtime used by MediaPipe:
sudo apt install ffmpeg libegl1 libgles2
```

The segmentation model (about 16 MB) is downloaded on first run to `~/.cache/phonk-edit/`.

### Usage

```sh
# quick 540x960 preview first
python phonk_edit.py clip.mp4 photo1.jpg photo2.jpg --audio track.mp3 --start 32 --duration 20 --scale 0.5 -o preview.mp4

# final render
python phonk_edit.py clip.mp4 photo1.jpg photo2.jpg --audio track.mp3 --start 32 --duration 20 -o edit.mp4
```

Inputs are used in order and repeat if there are more cuts than inputs. `--hero N` picks the
input shown on every drop (default: the first one).

| Option | Meaning |
|---|---|
| `--start`, `--duration` | Which part of the track to use. Pick a window that contains the drop. |
| `--bpm 140` | Exact tempo if the detected tempo is off (likely on very short windows). |
| `--drops 41.2,52.6` | Drop times in the track, if the detected ones are wrong. |
| `--neon "#ff2bd6,#00e5ff"` | Outline colours (one colour for a mono outline). |
| `--grade 0.7` | Colour grade strength. |
| `--mask-dir masks/` | Use your own `<photo-name>.png` masks (white = subject) instead of auto segmentation. |
| `--scale 0.5` | Fast half-resolution preview. |
| `--crf 17` | x264 quality (lower = better and bigger). |

A full 20 s render takes a few minutes on a laptop CPU; previews are about 4x faster.

### Music

Use a track you licensed, a royalty-free one, or your own. To try the template without any
music, `make_beat.py` synthesizes a royalty-free drift-phonk style beat (cowbell, 808 slides,
kick/clap, hats) with drops at bar 3 and bar 8:

```sh
python make_beat.py phonk.wav --bpm 130
```

### Tips

- Clips where the subject is clearly separated from the background give the cleanest outline.
- When several people are in the frame, the largest person near the centre gets the outline.
- TikTok/Reels/Shorts accept the output as is (H.264, AAC, yuv420p, faststart).

## Magyar

Függőleges, 1080x1920-as, ütemre vágott phonk editet készít fotókból és/vagy videókból meg egy
zenéből. Csak olyan felvételt használj, ami a tiéd, vagy amihez a szereplők hozzájárultak.

Tudja: neon körvonal a fő alak körül (haj is), pulzáló aura és részecskék, RGB split, glitch,
fehér villanások, rázkódás a basszusra, gyorsítás/lassítás és zoom elmosással, sötét filmes
színezés. A vágások és effektek a zene ütemeire kerülnek, a legerősebbek a dropokra.

```sh
pip install -r requirements.txt
sudo apt install ffmpeg libegl1 libgles2
python phonk_edit.py klip.mp4 foto1.jpg foto2.jpg --audio zene.mp3 --start 32 --duration 20 --scale 0.5 -o elonezet.mp4
python phonk_edit.py klip.mp4 foto1.jpg foto2.jpg --audio zene.mp3 --start 32 --duration 20 -o edit.mp4
```

Ha a tempó vagy a drop rossz helyre kerül: `--bpm 140`, illetve `--drops 41.2,52.6`.
Zene nélküli kipróbáláshoz: `python make_beat.py phonk.wav` (saját generálású, jogdíjmentes).
A többi kapcsoló a fenti angol táblázatban.
