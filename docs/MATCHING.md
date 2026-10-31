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
   Apple Log / Rec.2020, Blackmagic Film Gen5 / Wide Gamut, RED Log3G10 / REDWideGamutRGB, DJI D-Log / D-Gamut and
   DaVinci Intermediate / DaVinci Wide Gamut, plus Rec.709 and sRGB. Gamut matrices are derived from the published primaries; the AWG3 one is checked against
   ARRI's matrix in `engine_tests`. Which log curve a frame uses is hard to tell from the picture: the curves differ by
   about an exposure and contrast change. Metadata, the file name or the user decide first. Otherwise `rankLogCurves`
   scores every curve on the full frame: black floor, highlight clip level, how well the decode fits shot and read
   noise, exposure and skin hue. A curve needs 80% of the score to be used, and the panel shows it ("F-Log 92%").
   Below that, or if it doesn't win on most frames, the default is F-Log (`kGuessedLog`). It takes several frames, with
   their scores averaged, not added. On noisy synthetic scenes of 12 curves it is right on 16 of 48 and never wrong,
   and falls back on the rest. Real clips have no ground truth. Uncalibrated it read Artlist clip 587966 as Canon Log
   3 at 92%, though F-Log puts skin on the skin line (44 deg against 53) and clips less. It now falls back on all five
   real clips tried, and the Artlist file name rule still comes first.
   - **Artlist "LOG" clips** are re-exported through DaVinci Resolve as ProRes 422 HQ, tagged Rec.709, with no
     camera make, model or log tag. On six of them, F-Log reads closest to normal exposure: mean |exposure| 0.62 stops,
     against 0.91 for V-Log, 0.92 for S-Log3, 1.03 for LogC3 and Canon Log 3, and 1.41 for DaVinci Intermediate. That
     matches what Lucas saw by eye. A file name with "Artlist" and "LOG" therefore picks F-Log, and the panel shows
     "Artlist log → F-Log (best fit)". The black floors of these clips (about 0.10) also fit a curve with black at
     0.093, not DaVinci Intermediate (black at 0).
   - The "reaches the top codes" test is taken at the 99th percentile, so a clipped sun or speculars don't make a log
     frame display-referred. Artlist clip 6539291 was read as Rec.709 before this.
   - The curve's camera gamut is applied whether the curve came from metadata or was guessed. Most of the visible
     difference between curves comes from the curve, not the gamut. With an S-Log3 curve and F-Gamut, skin comes out
     like S-Log3/S-Gamut3.Cine; with an F-Log curve and S-Gamut3.Cine, like F-Log/F-Gamut.
2. **White balance.** The illuminant is estimated in scene linear from the most neutral fifth of the frame. "Most
   neutral" is ranked, not a fixed distance, so a grade's contrast or saturation doesn't change which pixels are
   picked. Clipped and very dark pixels are left out. It is iterated from a white-patch / grey-edge start, and blended
   with the white patch. The estimate is shown as CCT (K) and tint. Tint is the CIE 1960 uv distance from the
   Planckian locus ×1000, positive = magenta, relative to D65 (so D65 reads 6504 K / 0). The clip is adapted to D65
   with Bradford. How far is scaled by how neutral those pixels really are. A scene with nothing grey in it (a
   blue-hour sky, a red-lit stage) keeps its colour rather than having it taken for a cast. That measure doesn't
   change with a cast. An estimate that is both warm and magenta sits where skin and beige do, not where light sources
   do, so hardly any of such a cast is taken out: 5% once it reads 10 tint and 4600 K or warmer, fading back to all of it
   at 3 tint or 5600 K. On portrait 6439240 (3987 K, +13.8) taking out half turned the face ashy, doubled the colour
   variation on the cheek (grey flecks) and made 45% of the dark eye pixels cyan. Now it is 1.3× the plain decode and
   2.7%. A warm light on the locus, or a warm backdrop that reads as one, is still corrected in full.
   Before any of this, black bars are taken off the frame and off the reference (`cropBars`): rows and columns at
   the edges that are at least 99% below 0.03. A sequence can hand the effect a clip with pillarbox or letterbox
   bars. Read as picture, 6% of bars turned a 3.4-stop clip into 11 stops, bent the tone curve and made skin
   speckle (light noise ×2.6). Below 6 stops under the median nothing counts as tone, so a burnt-in border or
   logo plate can't do the same.
3. **Exposure.** The key is a trimmed log-average of luminance in linear light (5–95%, clipped pixels included, so
   they don't bias it). It is reported in stops from 18% grey.
4. **Tone.** One smooth curve on luminance, applied to R, G and B as a ratio, so hue and colour relationships stay
   the frame's own. Identity is the plain camera transform, so anything the curve leaves alone looks like it.
   - It is fitted to five levels of the tones the frame *occupies*, counted evenly across stops, not by pixel count.
     By pixel count, a plain backdrop filling most of a close-up owned the curve: on portrait 6439240, 66 % of the frame
     sat in a 0.75-stop band, its slope went from 2.7 to 0.36 within a quarter stop, and shaded skin got about 3×
     contrast next to flat lit skin.
   - The curve is a cubic, so it can make a gentle S with a toe and shoulder, its slope changes evenly and it can't
     kink. Its bend is scaled back until the slope stays between 0.6 and 1.6, and at most 1.05 across the skin in the
     frame: a slope above 1 on skin reads as crunch, so contrast comes from around the skin, not from steepening it.
     A match that would need more stops short of the reference.
   - **Exposure** is one global gain on top: skin lands at the reference's skin level (or 0.5 stop above its mid grey
     if it shows no skin); a frame without skin puts its mid grey on the reference's. The gain only lifts, by at most
     2 stops, because the fit is never too bright on its own.
   - The cost: a limited curve can't fully undo a strong curve baked into display-referred footage.
5. **Residual colour.** In OKLab:
   - **Skin weights are taken here.** They come after the scene-linear white balance and the tone normalisation, so a
     cast, an exposure or a curve doesn't move a face into or out of the mask. They come before the residual cast and
     saturation steps and the look, where a face stops looking like skin. The weight is a soft prior: hue around 52°
     (σ 22°), chroma above about 0.003–0.012 (pale skin highlights count), and not black or blown. Taken before the
     white balance instead, a cast moved faces in and out of the mask, and the harness mean went from 1.50 to 2.34.
   - A small ab offset (a grade's colour correction) is fitted robustly. It may vary with lightness, the most
     colourful pixels count least, skin counts 10%, and it is bounded and scaled by the white-balance confidence. A
     frame that is mostly skin (a close-up) has only part of it taken out, because it has no grey to judge a cast by.
     This is what turned the 8K portrait's skin grey (chroma 0.010).
   - saturation is brought to a canonical level from the 75th percentile of chroma, within ×0.6–×1.4. Skin keeps
     most of its own saturation.
6. **The look**, read from the reference thing by thing rather than as global statistics. Global statistics make a
   filter: whites are pushed wherever the mean says, and skin splits from everything else.
   - **Tone**: the reference's luminance distribution is where the tone lands. Its key is moderated: below −1 stop or
     above +0.5, only 55% of the extra distance is kept, by lifting shadows or taming highlights. So a night
     reference keeps a dark, moody exposure, but doesn't crush a bright day clip into mud.
   - **Neutral axis**: the tint of the reference's near-grey pixels in five lightness bands is its split tone. The
     clip's greys take exactly that, so whites get the reference's white.
   - **Skin** takes the reference's skin *treatment*, not the reference person's skin. Moving every face toward the
     reference's absolute skin saturation pushed a pale subject toward a deep complexion's: orange-red arms on the
     bedroom clip, matched to the street reference.
     - **Saturation, relative to the frame.** Skin keeps the reference's ratio of skin chroma to frame chroma (×0.8–1.25),
       held between 0.035 and 0.09.
     - **Hue.** It moves at most 6° toward the reference's skin and stays within 36–64°. Stray patches are pulled a
       quarter of the way to the face's own hue.
     - **The neutral axis.** Skin follows the grey axis only 10%, so the reference's tint of white doesn't make skin
       highlights pink or lavender.
     - **Lips** and strong reds next to skin keep their saturation.
   - **Other colours**: six hue sectors (red, yellow, green, cyan, blue, magenta). Where both frames have a sector
     (3% or more), it takes the reference's hue shift (within ±15°) and chroma (×0.8–1.25). Elsewhere colours only
     follow the overall saturation (×0.75–1.25).
   - Colours rotate and scale about the neutral axis, so none crosses it. Hue moves fade out below chroma 0.015–0.05,
     because a nearly grey wall's hue flips with every 8-bit step.
7. **Final tone match** (**Match Contrast**, default 100%). The graded result's occupied lightness levels are brought
   toward the reference's with the same kind of curve as step 4, on lightness alone (OKLab L; hue and chroma
   untouched). The slope is held between 0.85 and 1.15, and at most 1.03 around skin. Only the shape applies: where
   the anchor sits stays exposure's. No level moves more than 0.15.
   - Around the lightness that clearly coloured skin lands at, the curve may darken by at most 0.02. The limit depends
     on lightness only and changes slowly, so lightness stays monotonic. An earlier version shrank the darkening per
     colour by the skin weight. Near-greys carry the reference's tint, so that weight switched on and off along the
     grey ramp, and the ramp went darker by up to 12/255.
   - The bedroom clip's 95/99% levels went from 0.80/0.86 to 0.83/0.89, the same as the reference's.
   - The slope cap of 1.3 is set by banding. At 1.6, the Marsa sky's step share with the 20.12.03 reference went
     from 6% to 22%; at 1.3 it is 14%, inside the 15% limit.

Each step has an amount in **Source** (panel and Effect Controls): **Normalise Exposure**, **Normalise White
Balance**, **Normalise Contrast** (tone and saturation), **Look Strength** and **Match Contrast**. At 0% a step keeps the clip's own
exposure, white balance or contrast.

The whole chain is baked into a 65³ LUT over the clip's code values, with a 256-point input shaper. The shaper is a
third the identity and two thirds the frame's own value distribution, so the grid is dense where the picture is
(shadows of a dark clip, the middle band of log). The GPU filter applies the same shaper and LUT, and `gpu_tests`
checks it matches the CPU: 0.000/255. A copy saved into the project (**Save into Project**) is resampled to 33³ on the
same shaper, to keep projects small. Projects saved by earlier builds load unchanged: their Match Data has no shaper.

## Deband (contour rings in skies)

Compressed log footage stores a clear sky as a staircase of flat steps one or two code values high. For
example, the Marsa beach clip is 10-bit HEVC S-Log3. The grade expands log to display and makes the sky
4–7× steeper per channel, so each step becomes 1.3–3.5/255. Because R, G and B step on different rows,
the plateaus show as concentric coloured rings. Premiere already gives the effect 32-bit float frames on
both the GPU and the CPU path, so the rings were not caused by bit depth. They are in the source, and the
grade made them visible.

With **Fix Sky Banding** on (off by default, since it softens fine skin texture and grain), the effect smooths
those steps and the source's fine noise before the grade. The method starts from the
idea behind f3kdb / libplacebo `deband`, reimplemented here:

- For each pixel, it samples point-symmetric pairs of neighbours: 8 pairs within 40 px at 1080p for banding, and
  6 within 3 px for fine noise (both scaled with frame height).
- A pair counts by how far its midpoint is from the pixel (zero on a straight gradient), with each side within
  twice the threshold (0.012 code values for the far ring, 0.006 for the near ring, falling off from half of
  it). Pairs across an edge don't count, and the near ring leaves freckles and fine colour detail alone.
- Fine texture stays: the pixel's brightness relative to the average comes back, cored by 0.0015 code values, so
  grain and a compressed sky's steps stay out and real texture stays in. Texture around the size of grain
  can't be told from grain and sky steps, so it is reduced: random texture of RMS 0.0065 keeps about 83 %. On lit skin in 4K close-ups,
  pore-scale luma detail stays at about 97–99 %, and the finest level, which is mostly grain, at about 88–98 %.
- It adds a little triangular dither where it smoothed, the same on all three channels.

The first version weighted each neighbour on its own and dithered each channel separately. On shaded skin that
left a different lopsided average on every pixel, plus colour noise: at the Artlist clips' noise level, skin chroma
noise came out 2.1× the decoded clip, as red speckle in skin shadows. It was less visible on flat walls. The LUT
wasn't the cause: gains there are smooth and no higher on skin than on the wall. Now it is 0.63× lit and in shadow.

`lm::debandAt` and the Metal `deband()` use the same hash and sample order, so the GPU and the CPU
agree (`gpu_tests`: max 0.001/255). The GPU filter reads from a copy of the frame, because the
kernel grades in place.

`banding_tests` renders skies through the engine and measures, along each column, the share of the
gradient's change that happens in visible jumps (> 0.5/255, after averaging 17 px across) and the 99th
percentile of those jumps. Threshold: share < 15 % and jump < 1/255.

| sky (reference 20.12.03) | step share before | after | largest step before | after |
|---|---|---|---|---|
| synthetic S-Log3 sky, 10-bit | 13.0 % | 0.1 % | 1.27/255 | 0.66/255 |
| synthetic S-Log3 sky, 8-bit | 54.4 % | 1.0 % | 4.20/255 | 0.78/255 |
| Marsa beach frame (16-bit extract of the 10-bit clip) | 44.7 % | 14.6 % | 2.37/255 | 0.94/255 |

With reference 15.38.33, the Marsa frame goes from 43 % to 2.6 %.

`banding_tests` also renders a skin patch, lit and in shadow, with noise. Chroma noise after the match must stay
below the deband-off level and below 0.75× the decoded clip's (the old deband fails both).

On the real log frames, deband keeps at least 89 % of the high-pass energy. That energy includes the source's
grain, which is now removed on purpose, so the check is at 83 %.

Cost: the GPU takes 12.4 ms per 4K frame, including the copy. The CPU fallback rises from 39 to about
400 ms per 4K frame.

Tools: `tools/extract_frame16.swift` extracts a 16-bit frame from a clip. Sky frames go in
`tests/frames-sky/NAME-sky<W>x<H>.png`, which is git-ignored.

## Statistical transfers benchmarked (color-matcher methods)

I reimplemented color-matcher's methods from the published maths (it is GPL-3.0 and LookMatch is
closed source, so none of its code is used). They are internal options (`SceneMatchOptions::method`,
`consistency_tests --method N`), run after the source normalisation and before the per-sector look:

- **1, HM-MKL-HM.** Histogram match on lightness, then the Monge-Kantorovich linear map of the OKLab
  distribution, T = Σs^-½ (Σs^½ Σt Σs^½)^½ Σs^-½ (Pitié & Kokaram 2007), then a second lightness
  match. Both histogram matches go through the smooth, slope-capped `smoothMap` curve, not raw CDFs.
- **2.** The mean of method 1 and the thing-by-thing grade.
- **3, hybrid.** A gentle lightness histogram match (slope 0.6–1.6) and the chroma scaled by one
  factor to the reference's spread about neutral (isotropic MKL, clamped 0.85–1.3), then the
  thing-by-thing grade.

The color-matcher Python package itself (HM-MVGD-HM) runs as an external baseline only, through
`tools/colormatcher_bench.py`. It is fed the exact frames `consistency_tests --dump DIR` writes, in a
throwaway virtualenv; nothing is shipped or linked.

8 frames, reference 15.38.33, the average over the unclipped perturbation rows:

| method | mean ΔE00 | p95 | content preserved | grey-axis jump failures |
|---|---|---|---|---|
| 0, thing by thing (default) | 1.48 | 2.91 | 7/8 frames, all synthetic scenes | 16 |
| 1, HM-MKL-HM | 0.66 | 1.62 | **no**: the red stage's hues collapse | 87, and 56 greys that get darker as the input gets brighter |
| 2, mean of 0 and 1 | 0.95 | 1.94 | **no** | 35 |
| 3, hybrid | 1.39 | 2.97 | 7/8, all synthetic scenes | 21 |
| color-matcher HM-MVGD-HM (package) | 2.15 | 6.14 | **no**: cyan skin, posterised skies, ringed suns | (not measured) |

Method 1 is the most consistent, because a full statistical transfer makes every input look like the
reference's distribution. That is also why it fails: it is a filter, not a grade. Its 3×3 map couples
lightness with colour, so it pushes blacks to red and crosses neutral, and it erases scene colours. The
package does the same and posterises as well.

Method 3 avoids those failures but gains little: 1.39 against 1.48, with more grey-axis steps. So the
default stays method 0, and methods 1–3 stay internal for the harness. The contact sheet (source |
LookMatch | color-matcher) was checked by eye: LookMatch keeps skin, skies and the sun clean;
color-matcher does not.

## Camera curves and gamuts against ground truth

`tests/check_transfers.py` compares every log decode and gamut matrix with colour-science (BSD-3;
test-only, `pip install colour-science`). It reads them through `lookmatch-render --dump-transfers`
and samples 65 code values per curve. All 11 log curves match to within 0.001% relative:

- S-Log3, LogC3, LogC4 and Canon Log 3
- V-Log, F-Log, F-Log2 and Apple Log
- BMD Film Gen5, Log3G10 and D-Log

The gamut matrices to linear Rec.709 (Bradford where the white point differs) match to within 2e-4.
The largest differences are Blackmagic Wide Gamut and DJI D-Gamut, both from rounded primaries.

## Gamut compression

The matcher's two conversions back from OKLab (the normalised frame, and the final look) used to clip
out-of-gamut colours channel by channel. Clipping shifts hue and leaves flat patches where
white-balance or saturation moves push colours out of gamut. They now use the ACES Reference Gamut
Compression (`gamutCompress` in color.h), implemented from the published specification:

- A = max(R, G, B), and per channel d = (A − c)/A.
- Above the threshold t = (0.815, 0.803, 0.880), d is compressed so that the limit l = (1.147, 1.264,
  1.312) lands on the gamut boundary, with power 1.2.

It is applied in linear Rec.709, before the display encode.

Measured on 8 frames: consistency is unchanged, at mean 1.48 / p95 2.86 against 2.91 before.
Grey-axis jump failures fall from 16 to 13. Content preservation is unchanged.

## Already in the design

Three of the open-source techniques reviewed were already how the matcher works, so they were not
added again:

- **Illuminant from surfaces and from edges** (darktable colour calibration). `estimateIlluminant`
  uses the most neutral pixels (iterated) together with a white patch and grey-edge. It leaves out
  clipped, very dark, skin-like and strongly coloured pixels.
- **Cluster pairing with soft weights** (darktable colour mapping). The look is moved per hue
  sector, using overlapping smooth weights. A sector moves only when the reference has evidence for
  it, and colours move about the neutral axis.
- **Hue-preserving tone curves on a norm** (AgX / sigmoid). The tone match is applied to luminance
  as a ratio, so it does not skew hues.

## Tried and not adopted

- **CAT16 instead of Bradford for the white-balance correction.** Over 8 frames, every white-balance
  row got slightly worse. For example, 4300 K tint −20 went from 0.12 to 0.44 mean ΔE00, and
  4300 K tint 0 from 0.16 to 0.29. The test's simulated casts are Bradford, so this measure favours
  Bradford, and there is no evidence from real footage either way. Bradford stays.
- **Statistical transfers as the default** (see above): more consistent, but they erase scene
  colour and break the grey axis.

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
| exposure | -2.0 stops | 0.40 | 0.88 | 4.33 / 7.49 | pass |
| exposure | -1.0 stops | 0.97 | 1.65 | 2.00 / 3.60 | pass |
| exposure | -0.5 stops | 0.73 | 1.28 | 0.99 / 1.82 | pass |
| exposure | +0.5 stops (23 frames clipped, below) | 0.11 | 0.48 | 1.67 / 5.21 | pass |
| exposure | +0.5 stops (clipped frames) | 1.56 | 4.93 |  | pass |
| exposure | +1.0 stops (25 frames clipped, below) | 0.17 | 0.62 | 5.17 / 11.54 | pass |
| exposure | +1.0 stops (clipped frames) | 5.46 | 13.54 |  | **fail** |
| exposure | +2.0 stops (clipped frames) | 9.11 | 19.70 | 9.88 / 17.86 | **fail** |
| white balance | 3200K tint +0 (23 frames clipped, below) | 0.29 | 1.17 | 11.06 / 21.48 | pass |
| white balance | 3200K tint +0 (clipped frames) | 4.61 | 12.56 |  | **fail** |
| white balance | 4300K tint +0 (21 frames clipped, below) | 0.18 | 0.52 | 8.42 / 14.99 | pass |
| white balance | 4300K tint +0 (clipped frames) | 1.15 | 3.59 |  | pass |
| white balance | 5500K tint +0 (10 frames clipped, below) | 0.13 | 0.37 | 4.98 / 8.26 | pass |
| white balance | 5500K tint +0 (clipped frames) | 0.36 | 0.80 |  | pass |
| white balance | 7500K tint +0 (7 frames clipped, below) | 0.14 | 0.38 | 4.12 / 6.93 | pass |
| white balance | 7500K tint +0 (clipped frames) | 0.70 | 2.24 |  | pass |
| white balance | 6504K tint +20 (17 frames clipped, below) | 0.66 | 1.24 | 8.37 / 13.59 | pass |
| white balance | 6504K tint +20 (clipped frames) | 1.47 | 5.35 |  | pass |
| white balance | 6504K tint -20 (3 frames clipped, below) | 0.20 | 0.51 | 9.83 / 15.75 | pass |
| white balance | 6504K tint -20 (clipped frames) | 0.34 | 0.66 |  | pass |
| white balance | 3200K tint +10 (23 frames clipped, below) | 4.93 | 9.82 | 13.56 / 25.72 | **fail** |
| white balance | 3200K tint +10 (clipped frames) | 7.15 | 14.42 |  | **fail** |
| white balance | 7500K tint -10 (3 frames clipped, below) | 0.15 | 0.39 | 6.63 / 11.11 | pass |
| white balance | 7500K tint -10 (clipped frames) | 0.19 | 0.43 |  | pass |
| white balance | 4300K tint -20 (11 frames clipped, below) | 0.32 | 0.78 | 8.91 / 14.56 | pass |
| white balance | 4300K tint -20 (clipped frames) | 0.51 | 1.07 |  | pass |
| curves | S-curve (2 frames clipped, below) | 2.98 | 6.01 | 2.71 / 4.81 | **fail** |
| curves | S-curve (clipped frames) | 2.63 | 5.37 |  | pass |
| curves | strong S-curve (4 frames clipped, below) | 4.58 | 9.00 | 4.59 / 7.75 | **fail** |
| curves | strong S-curve (clipped frames) | 4.55 | 8.77 |  | pass |
| curves | inverse S | 3.26 | 6.48 | 2.09 / 4.12 | **fail** |
| curves | contrast +25% (21 frames clipped, below) | 3.54 | 8.88 | 2.42 / 6.28 | **fail** |
| curves | contrast +25% (clipped frames) | 3.37 | 9.24 |  | pass |
| curves | lift +0.08 | 1.58 | 3.43 | 0.84 / 1.65 | pass |
| curves | gamma 0.8 | 1.39 | 2.60 | 1.82 / 3.05 | pass |
| curves | gamma 1.25 | 1.33 | 2.43 | 1.54 / 2.73 | pass |
| curves | gain 0.85 | 0.99 | 1.79 | 0.97 / 1.73 | pass |
| curves | colour LGG (warm gain, blue lift) (13 frames clipped, below) | 3.71 | 7.87 | 7.67 / 12.49 | **fail** |
| curves | colour LGG (warm gain, blue lift) (clipped frames) | 3.43 | 6.76 |  | pass |
| curves | faded (lift + gain) | 2.70 | 5.55 | 1.56 / 2.78 | **fail** |
| saturation | saturation 0.50 | 3.16 | 6.33 | 4.29 / 7.24 | **fail** |
| saturation | saturation 0.75 | 1.49 | 3.07 | 1.79 / 3.10 | pass |
| saturation | saturation 1.25 (1 frames clipped, below) | 1.22 | 2.58 | 1.33 / 2.24 | pass |
| saturation | saturation 1.25 (clipped frames) | 1.16 | 2.50 |  | pass |
| saturation | saturation 1.50 (2 frames clipped, below) | 2.06 | 4.27 | 2.18 / 3.72 | **fail** |
| saturation | saturation 1.50 (clipped frames) | 1.88 | 4.33 |  | pass |
| log | as S-Log3 | 0.23 | 0.81 | 7.49 / 13.69 | pass |
| log | as LogC3 | 1.00 | 2.22 | 7.32 / 13.50 | pass |
| log | as LogC4 | 2.14 | 4.31 | 8.29 / 14.66 | **fail** |
| log | as Canon Log 3 | 1.95 | 3.93 | 7.67 / 13.25 | pass |
| log | as V-Log | 1.40 | 2.99 | 7.92 / 14.28 | pass |
| log | as F-Log | 2.62 | 4.71 | 6.57 / 11.80 | **fail** |
| log | as F-Log2 | 1.84 | 4.10 | 7.62 / 13.96 | pass |
| log | as Apple Log | 1.93 | 4.08 | 7.22 / 13.16 | pass |
| log | as BMD Film Gen5 | 1.72 | 3.13 | 7.80 / 14.12 | pass |
| log | as Log3G10 | 1.99 | 3.76 | 8.14 / 14.39 | pass |
| log | as D-Log | 0.67 | 1.56 | 7.43 / 13.72 | pass |
| combined | -1 stop, 3200K, S-curve | 5.17 | 10.67 | 13.68 / 24.21 | **fail** |
| combined | +0.5 stop, 7500K tint -10, sat 0.7 | 3.31 | 6.83 | 7.40 / 12.70 | **fail** |
| combined | lift, gamma 1.2, 5500K, sat 1.3 (13 frames clipped, below) | 1.38 | 3.20 | 6.09 / 10.07 | pass |
| combined | lift, gamma 1.2, 5500K, sat 1.3 (clipped frames) | 2.18 | 4.40 |  | pass |
| combined | S-Log3 shot -1 stop at 4300K | 0.42 | 1.20 | 9.25 / 14.98 | pass |
| combined | LogC3 shot +1 stop at 7500K tint +10 | 1.24 | 2.64 | 7.73 / 13.62 | pass |
| combined | V-Log shot -0.5 stop, tint -15 | 1.44 | 3.00 | 8.44 / 15.30 | pass |
| combined | inverse S, gain 0.9, 4300K tint +10 (2 frames clipped, below) | 5.81 | 10.77 | 11.60 / 20.66 | **fail** |
| combined | inverse S, gain 0.9, 4300K tint +10 (clipped frames) | 5.70 | 10.06 |  | **fail** |


Other checks on the same run:

- **No NaNs.**
- **Banding:** the LUT never adds steps the transform doesn't have. The largest LUT-vs-exact local gain on smooth
  gradients is 1.61, against a bound of 1.25× the exact gain + 1.5. The grey axis still fails on a few extreme
  perturbations: steps up to 34/255 per 1/1024, and slight darkening.
- **Content preservation:** 27 of 29 frames (26 before the skin work).
- **Log detection:** all 286 log-encoded frames were detected as log. 15 display-referred perturbed frames were
  taken for log (12 before the top codes were judged at the 99th percentile; that is what catches log clips with a
  clipped sun).
- **Skin** (11 frames with skin, judged where skin is at most 40% of the frame): no face flagged ashy, orange or
  split. The two flags are Clip 14, sunlit concrete that the hue prior reads as skin. Mean skin-only ΔE00 against
  the untouched match:

  | Perturbation | ΔE00 |
  |---|---|
  | exposure | 0.95 |
  | white balance | 0.92 |
  | log | 2.00 |
  | saturation | 2.64 |
  | curves | 2.88 |
  | combined | 3.02 |

- **Cost of the skin rules:** consistency on unclipped rows is 1.73 / 3.57 on average, against 1.71 / 3.35 before,
  with 19 rows over target instead of 21. The loss is on warm-magenta casts, which the white balance only half
  removes because they look like skin: 3200 K tint +10 went from 3.20 to 4.93, and inverse S with 4300 K tint +10
  from 5.13 to 5.81. Without the skin rules those rows return. They are kept because they are what fixed orange,
  grey and split skin on real footage.
- **GPU vs CPU:** 0.000/255 (`gpu_tests`).

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

## Skin tone evening

Pink and grey patches in skin are in the clip: they stay put from frame to frame, and brighter bumps are greyer. A lift
makes them easier to see. With a match on, **Skin Evening** (default 50%) moves each skin pixel's colour part of the way
to the skin around it, in the source code values before the grade, on the CPU and the GPU alike:

- Colour here is the pixel minus its mean of R, G and B, so brightness doesn't change and pores and texture stay.
- The average is over 24 neighbours on a disc of 240 px at 8K, scaled with frame height. A neighbour counts only if it
  is skin, close in brightness and close in colour.
- The mask is by colour only: warm, between lips' red and wood's yellow, with a little colour. A flat beige backdrop
  inside it evens toward itself, so it hardly moves.

The match cache key includes a hash of `src/core`, so an engine change misses the cache.
