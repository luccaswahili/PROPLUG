# Source-aware matching

The matcher used to apply a reference's look to whatever the frame looked like: a clip shot in log, one exposed a stop
under, one balanced for tungsten or one already graded with an S-curve each came out differently. Now the clip is
analysed first and brought to one canonical state, and the reference's look is applied to that state. The match then
depends on what is in the shot, not on how it was shot or graded before.

Code: `src/core/scene.{h,cpp}` (curves, gamuts, white points, analysis) and `matchScene` in `src/core/matcher.cpp`.
The Premiere effect calls it when it builds a match. The CPU and GPU paths both use the resulting LUT.

## The chain

1. **Input transfer.** Clip metadata wins: the panel reads the clip's colour space, input LUT and file name, and the
   **Source Format** control can name the curve. Otherwise the frame is classified from its statistics. Log has a
   raised black floor (≥ 0.05–0.09), highlights well short of the top codes, a narrow histogram and low saturation.
   A faded grade lifts blacks too, but it keeps its contrast, highlights or colour. A frame that reaches the top codes
   or true black is display-referred. Supported curves, each with its camera gamut: S-Log3 / S-Gamut3.Cine,
   LogC3 / AWG3, LogC4 / AWG4, Canon Log 3 / Cinema Gamut, V-Log / V-Gamut, F-Log and F-Log2 / F-Gamut,
   Apple Log / Rec.2020, Blackmagic Film Gen5 / Wide Gamut, RED Log3G10 / REDWideGamutRGB and DJI D-Log / D-Gamut,
   plus Rec.709 and sRGB. Gamut matrices are derived from the published primaries; the AWG3 one is checked against
   ARRI's matrix in `engine_tests`. Which log curve a frame uses can't be told from the picture: the curves differ by
   about an exposure and contrast change. So a detected log clip is decoded with S-Log3 unless metadata or the user
   says otherwise, and the following steps absorb most of the difference.
2. **White balance.** The illuminant is estimated in scene linear from the most neutral fifth of the frame. "Most
   neutral" is ranked, not a fixed distance, so a grade's contrast or saturation doesn't change which pixels are
   picked. Clipped and very dark pixels are left out. It is iterated from a white-patch / grey-edge start, and blended
   with the white patch. The estimate is shown as CCT (K) and tint. Tint is the CIE 1960 uv distance from the
   Planckian locus ×1000, positive = magenta, relative to D65 (so D65 reads 6504 K / 0). The clip is adapted to D65
   with Bradford. How far is scaled by how neutral those pixels really are. A scene with nothing grey in it (a
   blue-hour sky, a red-lit stage) keeps its colour rather than having it taken for a cast. That measure doesn't
   change with a cast. An estimate that is both warm and magenta sits where skin does, not where light sources do,
   so only half of such a cast is taken out. Otherwise a close-up's face would be neutralised.
3. **Exposure.** The key is a trimmed log-average of luminance in linear light (5–95%, clipped pixels included, so
   they don't bias it). It is reported in stops from 18% grey.
4. **Tone.** The luminance distribution in log2 stops is mapped by rank to where the look puts it. With full
   contrast normalisation that is a fixed canonical distribution: normal, σ = 1.6 stops. It is applied to R, G and B
   as a luminance ratio, so hue and colour relationships stay the frame's own. A curve a grade or transfer applied
   keeps the order of the pixels' brightness, so this undoes it.
   - The curve is a monotone cubic spline through 17 rank-averaged control points, not the raw quantiles. Read
     straight off an 8-bit frame, the quantiles form a staircase, and the staircase posterises.
   - Its slope is held between 0.35 and 2.2.
   - The frame's 5–95% range is stretched at most 2×: a soft close-up stays soft.
   - True black meets the curve's floor continuously.
5. **Residual colour.** In OKLab:
   - a small ab offset (a grade's colour correction) is fitted robustly. It may vary with lightness, the most
     colourful pixels count least, and it is bounded and scaled by the white-balance confidence;
   - saturation is brought to a canonical level from the 75th percentile of chroma, within ×0.6–×1.4. Skin keeps
     most of its own saturation.
6. **The look**, read from the reference thing by thing rather than as global statistics. Global statistics make a
   filter: whites are pushed wherever the mean says, and skin splits from everything else.
   - **Tone**: the reference's luminance distribution is where the tone lands. Its key is moderated: below −1 stop or
     above +0.5, only 55% of the extra distance is kept, by lifting shadows or taming highlights. So a night
     reference keeps a dark, moody exposure, but doesn't crush a bright day clip into mud.
   - **Neutral axis**: the tint of the reference's near-grey pixels in five lightness bands is its split tone. The
     clip's greys take exactly that, so whites get the reference's white.
   - **Skin**: if the reference has skin, the clip's skin moves toward its hue (within ±6°) and chroma (×0.85–1.1).
     If it has none, skin keeps its hue.
   - **Other colours**: six hue sectors (red, yellow, green, cyan, blue, magenta). Where both frames have a sector
     (3% or more), it takes the reference's hue shift (within ±15°) and chroma (×0.8–1.25). Elsewhere colours only
     follow the overall saturation (×0.75–1.25).
   - Colours rotate and scale about the neutral axis, so none crosses it. Hue moves fade out below chroma 0.015–0.05,
     because a nearly grey wall's hue flips with every 8-bit step.

Each step has an amount in **Source** (panel and Effect Controls): **Normalise Exposure**, **Normalise White
Balance**, **Normalise Contrast** (tone and saturation) and **Look Strength**. At 0% a step keeps the clip's own
exposure, white balance or contrast.

The whole chain is baked into a 65³ LUT over the clip's code values, with a 256-point input shaper. The shaper is a
third the identity and two thirds the frame's own value distribution, so the grid is dense where the picture is
(shadows of a dark clip, the middle band of log). The GPU filter applies the same shaper and LUT, and `gpu_tests`
checks it matches the CPU: 0.000/255. A copy saved into the project (**Save into Project**) is resampled to 33³ on the
same shaper, to keep projects small. Projects saved by earlier builds load unchanged: their Match Data has no shaper.

## Consistency test

`tests/consistency_tests.cpp` (ctest `consistency_tests`) takes real frames from the clips in projects *g* and *d*:
26 frames, 2 per clip at 640 px, made with `tools/extract_frames.swift`. They are kept out of git. The reference is
Lucas's "Screenshot 2026-10-29 at 15.38.33.png". Its look, as the transfer reads it (`describeLook`):

- Neutral axis: almost untinted. Greys carry ≤ 0.006 OKLab chroma from shadows to whites, drifting faintly cool
  (hue 180–250°). Blacks are a touch warm (0.010 at 85°).
- Skin: 8% of the frame, hue 51° (warm tan), chroma 0.065.
- Saturation: median chroma 0.058.
- Colour sectors: a teal sky (20%, hue 219°, chroma 0.058), blue 6% (hue 251°) and red 6% (hue 31°, chroma 0.081).
- Tone: OKLab lightness 2/50/98% = 0.14 / 0.59 / 0.84, so open blacks and highlights rolled off below white.
  Contrast is 4.9 stops from the 5th to the 95th percentile.

Lucas's own LOG clips were checked by eye on a contact sheet (reference, source and output for 12 frames) and in
Premiere. They are not in the automated set, as there is no "untouched" grade of them to compare against. Each frame is perturbed:

- exposure −2 … +2 stops
- white balance 3200 K … 7500 K and tint ±20, as Bradford casts in linear light
- S / strong S / inverse S curves, contrast +25%, lift, gamma 0.8 / 1.25, gain, a per-channel lift/gamma/gain grade,
  and a faded look
- saturation 0.5 … 1.5, in linear light around luminance
- re-encoded into each of the 11 log curves, with its gamut
- combinations of these

Each perturbed frame is matched to the reference and compared, in CIEDE2000, with the match of the untouched frame.
The target is mean ≤ 2 and 95th percentile ≤ 5. A frame where a perturbation clipped more than 1% of its pixels is
reported separately, with a looser bound (mean ≤ 5, p95 ≤ 12): that information is gone.

The test also checks the following:

- no NaNs
- a monotonic grey axis
- no banding: on a heavily blurred copy of each frame, the LUT's local gain must stay within 1.25× (+1.5) of the
  exact transform's
- content preservation: on the real frames and on three strongly coloured synthetic scenes (forest, blue hour, red
  stage), every hue sector holding 3% of the colourful pixels keeps its place relative to the others and stays
  coloured

`--old` adds the previous matcher's numbers.

## Results (26 frames, reference 15.38.33, 65³ LUT as the effect builds it)

Measured with `consistency_tests --old` on this branch. "Previous matcher" is the matcher on main, run on the same
frames and perturbations. Rows marked *(clipped frames)* are the frames a perturbation clipped (more than 1% of the
pixels), with the looser bound.

| Group | Perturbation | Mean ΔE00 | p95 ΔE00 | Previous matcher (mean / p95) | Result |
|---|---|---|---|---|---|
| exposure | -2.0 stops | 0.44 | 0.92 | 4.33 / 7.49 | pass |
| exposure | -1.0 stops | 1.04 | 1.77 | 2.00 / 3.60 | pass |
| exposure | -0.5 stops | 0.48 | 0.86 | 0.99 / 1.82 | pass |
| exposure | +0.5 stops (23 frames clipped, below) | 0.05 | 0.15 | 1.67 / 5.21 | pass |
| exposure | +0.5 stops (clipped frames) | 1.84 | 4.88 |  | pass |
| exposure | +1.0 stops (25 frames clipped, below) | 0.07 | 0.10 | 5.17 / 11.54 | pass |
| exposure | +1.0 stops (clipped frames) | 5.05 | 12.74 |  | **fail** |
| exposure | +2.0 stops (clipped frames) | 9.12 | 19.62 | 9.88 / 17.86 | **fail** |
| white balance | 3200K tint +0 (23 frames clipped, below) | 0.15 | 0.51 | 11.06 / 21.48 | pass |
| white balance | 3200K tint +0 (clipped frames) | 4.76 | 12.54 |  | **fail** |
| white balance | 4300K tint +0 (21 frames clipped, below) | 0.12 | 0.28 | 8.42 / 14.99 | pass |
| white balance | 4300K tint +0 (clipped frames) | 1.08 | 3.67 |  | pass |
| white balance | 5500K tint +0 (10 frames clipped, below) | 0.08 | 0.22 | 4.98 / 8.26 | pass |
| white balance | 5500K tint +0 (clipped frames) | 0.27 | 0.66 |  | pass |
| white balance | 7500K tint +0 (7 frames clipped, below) | 0.09 | 0.24 | 4.12 / 6.93 | pass |
| white balance | 7500K tint +0 (clipped frames) | 0.53 | 1.80 |  | pass |
| white balance | 6504K tint +20 (17 frames clipped, below) | 0.54 | 1.20 | 8.37 / 13.59 | pass |
| white balance | 6504K tint +20 (clipped frames) | 1.24 | 4.71 |  | pass |
| white balance | 6504K tint -20 (3 frames clipped, below) | 0.13 | 0.30 | 9.83 / 15.75 | pass |
| white balance | 6504K tint -20 (clipped frames) | 0.19 | 0.48 |  | pass |
| white balance | 3200K tint +10 (23 frames clipped, below) | 3.34 | 6.81 | 13.56 / 25.72 | **fail** |
| white balance | 3200K tint +10 (clipped frames) | 6.90 | 14.13 |  | **fail** |
| white balance | 7500K tint -10 (3 frames clipped, below) | 0.11 | 0.27 | 6.63 / 11.11 | pass |
| white balance | 7500K tint -10 (clipped frames) | 0.13 | 0.29 |  | pass |
| white balance | 4300K tint -20 (11 frames clipped, below) | 0.16 | 0.39 | 8.91 / 14.56 | pass |
| white balance | 4300K tint -20 (clipped frames) | 0.47 | 1.04 |  | pass |
| curves | S-curve (2 frames clipped, below) | 3.43 | 5.85 | 2.71 / 4.81 | **fail** |
| curves | S-curve (clipped frames) | 3.14 | 4.79 |  | pass |
| curves | strong S-curve (4 frames clipped, below) | 5.66 | 9.47 | 4.59 / 7.75 | **fail** |
| curves | strong S-curve (clipped frames) | 4.88 | 8.41 |  | pass |
| curves | inverse S | 3.20 | 5.62 | 2.09 / 4.12 | **fail** |
| curves | contrast +25% (21 frames clipped, below) | 4.47 | 8.72 | 2.42 / 6.28 | **fail** |
| curves | contrast +25% (clipped frames) | 4.08 | 10.17 |  | pass |
| curves | lift +0.08 | 1.86 | 3.52 | 0.84 / 1.65 | pass |
| curves | gamma 0.8 | 1.65 | 3.03 | 1.82 / 3.05 | pass |
| curves | gamma 1.25 | 1.52 | 2.78 | 1.54 / 2.73 | pass |
| curves | gain 0.85 | 0.80 | 1.45 | 0.97 / 1.73 | pass |
| curves | colour LGG (warm gain, blue lift) (13 frames clipped, below) | 2.61 | 6.40 | 7.67 / 12.49 | **fail** |
| curves | colour LGG (warm gain, blue lift) (clipped frames) | 2.88 | 5.91 |  | pass |
| curves | faded (lift + gain) | 2.97 | 5.36 | 1.56 / 2.78 | **fail** |
| saturation | saturation 0.50 | 3.47 | 7.37 | 4.29 / 7.24 | **fail** |
| saturation | saturation 0.75 | 1.61 | 3.53 | 1.79 / 3.10 | pass |
| saturation | saturation 1.25 (1 frames clipped, below) | 1.20 | 2.76 | 1.33 / 2.24 | pass |
| saturation | saturation 1.25 (clipped frames) | 1.19 | 3.00 |  | pass |
| saturation | saturation 1.50 (2 frames clipped, below) | 2.25 | 4.90 | 2.18 / 3.72 | **fail** |
| saturation | saturation 1.50 (clipped frames) | 1.77 | 4.53 |  | pass |
| log | as S-Log3 | 0.16 | 0.50 | 7.49 / 13.69 | pass |
| log | as LogC3 | 0.89 | 1.96 | 7.32 / 13.50 | pass |
| log | as LogC4 | 2.19 | 4.35 | 8.29 / 14.66 | **fail** |
| log | as Canon Log 3 | 2.30 | 4.18 | 7.67 / 13.25 | **fail** |
| log | as V-Log | 1.38 | 2.93 | 7.92 / 14.28 | pass |
| log | as F-Log | 2.89 | 5.00 | 6.57 / 11.80 | **fail** |
| log | as F-Log2 | 1.79 | 4.04 | 7.62 / 13.96 | pass |
| log | as Apple Log | 1.90 | 4.14 | 7.22 / 13.16 | pass |
| log | as BMD Film Gen5 | 1.92 | 3.50 | 7.80 / 14.12 | pass |
| log | as Log3G10 | 2.14 | 4.19 | 8.14 / 14.39 | **fail** |
| log | as D-Log | 0.61 | 1.34 | 7.43 / 13.72 | pass |
| combined | -1 stop, 3200K, S-curve | 5.71 | 11.18 | 13.68 / 24.21 | **fail** |
| combined | +0.5 stop, 7500K tint -10, sat 0.7 | 2.77 | 6.38 | 7.40 / 12.70 | **fail** |
| combined | lift, gamma 1.2, 5500K, sat 1.3 (13 frames clipped, below) | 2.05 | 4.01 | 6.09 / 10.07 | **fail** |
| combined | lift, gamma 1.2, 5500K, sat 1.3 (clipped frames) | 2.28 | 4.67 |  | pass |
| combined | S-Log3 shot -1 stop at 4300K | 0.26 | 0.74 | 9.25 / 14.98 | pass |
| combined | LogC3 shot +1 stop at 7500K tint +10 | 1.04 | 2.24 | 7.73 / 13.62 | pass |
| combined | V-Log shot -0.5 stop, tint -15 | 1.40 | 2.93 | 8.44 / 15.30 | pass |
| combined | inverse S, gain 0.9, 4300K tint +10 (2 frames clipped, below) | 5.13 | 8.98 | 11.60 / 20.66 | **fail** |
| combined | inverse S, gain 0.9, 4300K tint +10 (clipped frames) | 3.54 | 5.65 |  | pass |


Other checks on the same run:

- **No NaNs.**
- **Banding:** the LUT never adds steps the transform doesn't have. The largest LUT-vs-exact local gain on smooth
  gradients is 1.61, against a bound of 1.25× the exact gain + 1.5. The grey axis still fails on a few extreme
  perturbations: steps up to 34/255 per 1/1024, and slight darkening.
- **Content preservation:** 26 of 29 frames.
- **Log detection:** all 286 log-encoded frames were detected as log. 12 display-referred perturbed frames were
  taken for log.
- **GPU vs CPU:** 0.000/255 ().

### Open cases

What passes: white balance, exposure up to +1 stop, most log encodings and most single curves. Three kinds of case
still miss the target:

- **Strong per-channel curves** (S, inverse S, contrast +25%, faded), saturation ×0.5 and stacked grades. A curve
  applied separately to R, G and B changes saturation by lightness and by colour. The shared tone curve is a
  luminance ratio, which keeps colours from flattening and splitting (see Content preservation), so it can't fully
  undo that. Undoing it would also need local contrast above the 2.2× cap, and the cap is what keeps 8-bit log
  close-ups free of banding.
- **Some log curves decoded as S-Log3** (F-Log, LogC4, Canon Log 3, Log3G10): about 2–3 mean ΔE00. Picking the
  right curve from metadata, or with **Source Format**, removes it: with the true curve, every log encoding is
  ≤ 0.2 / 0.7.
- **Clipped extremes** (+2 stops, 3200 K): information is gone.
