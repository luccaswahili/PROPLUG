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
   change with a cast.
3. **Exposure.** The key is a trimmed log-average of luminance in linear light (5–95%, clipped pixels included, so
   they don't bias it). It is reported in stops from 18% grey.
4. **Tone.** The luminance distribution, in log2 stops, is mapped by rank to a fixed canonical distribution: normal,
   σ = 1.6 stops. It is applied to R, G and B as a luminance ratio, so hue and colour relationships stay the frame's
   own. Any curve a grade or transfer applied keeps the order of the pixels' brightness, so this undoes it. The curve
   is monotonic, its slope is held between 0.33 and 3, and a lift of the darkest values fades out below them. These
   are the same caps that fixed the blocky shadows.
5. **Residual colour.** In OKLab: a small ab offset (a grade's colour correction) is fitted robustly. It may vary with
   lightness, the most colourful pixels count least, and it is bounded and scaled by the white-balance confidence.
   The saturation is then brought to a canonical level from the 75th percentile of chroma, within ×0.5–×2.
6. **The look.** The reference goes through the same analysis. Its luminance distribution is where the tone lands.
   Its key is moderated: below −1 stop or above +0.5, only 55% of the extra distance is kept, by lifting shadows or
   taming highlights. So a night reference keeps a dark, moody exposure, but doesn't crush a bright day clip into mud.
   Colour is the prototype's region-aware transfer, run on the canonical frame: Monge–Kantorovich chroma, region
   offsets (skin to skin, sky to sky), split toning per tonal band and skin protection.

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
Lucas's "Screenshot 2026-10-29 at 15.38.33.png". Each frame is perturbed:

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

RESULTS
