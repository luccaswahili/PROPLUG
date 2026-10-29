# LookMatch (native)

A native effect for Premiere Pro (and After Effects). It matches the look of any reference still,
source-aware: each clip's format (log or display), exposure, white balance and contrast are evened out
first, so the match depends on what is in the shot, not on how it was shot or graded. It finishes the
shot with halation, bloom, grain and (in Effect Controls) film print emulation. The C++ engine renders
every frame itself, like QG PrintGrade does. It is not a pack of Lumetri presets. An **Export .cube**
button also bakes the colour part into a LUT for Lumetri.

## What's in the box

| Section | Controls |
|---|---|
| Look Match | Load Reference..., Match to This Frame, Match Strength, Region Matching, Auto Skin Protect / Skin Protect |
| Input | Footage Encoding (Rec.709, ARRI LogC3, Sony S-Log3, Panasonic V-Log), Exposure, Temperature, Tint |
| Film Print | Film Strength, Negative Stock (7), Print Stock (5), Printer Lights R/G/B, Film Saturation, Fade |
| Film Texture | Halation (+ size, threshold), Bloom (+ size, threshold), Grain (+ size, colour), Softness, Vignette, Gate Weave, Flicker |
| Output | Mix, Export .cube... |
| Source | Source Format (Auto, Rec.709, sRGB, S-Log3, LogC3, LogC4, Canon Log 3, V-Log, F-Log, F-Log2, Apple Log, BMD Film Gen5, RED Log3G10, DJI D-Log), Normalise Exposure / White Balance / Contrast, Look Strength |

**Negative stocks:** Tungsten 500T, Daylight 250D, Fine Grain 50D, Portrait 400, Low-Con Neutral,
Night 800T (heavy halation), Black & White 400.
**Print stocks:** None (scanned negative), Cine Print Warm, Cine Print Cool, Low-Con Print, High-Con Print.
The stocks are original approximations of familiar film families, tuned by eye. They are not
measured emulations of any manufacturer's product. The stock you pick also scales grain amount,
grain size and halation: B&W has no red halation, and 800T has a lot.

## How it renders a frame

```
input ──► gate weave ──► decode to scene linear ──► halation / bloom / softness ──► colour LUT ──► grain / vignette / mix ──► output
          (per frame)    (+ exposure, WB, flicker)    (in linear light, 1/4 res blur)    (65³, rebuilt only     (display space)
                                                                                         when colour changes)
```

The colour LUT combines four steps: the neutral display transform, the look match, the film
negative→print model, and fade. The film model is a two-stage density model: per-layer H&D curves
with interlayer coupling on the negative, printed through printer lights onto a print stock with
dye crosstalk. That gives film's toe, shoulder and colour crossover, and printer-point grading.

The look match is source-aware (`src/core/scene.*`, `matchScene` in `src/core/matcher.cpp`). The clip
is decoded (its log curve and gamut, detected or named), white balanced in scene linear, its tone curve
undone by rank to a canonical distribution, and its saturation evened out. Then the reference's look is
applied: its tone distribution (with its key moderated, so a night reference doesn't crush a day clip)
and the region-aware colour transfer from the Python prototype, which matches skin to skin, sky to sky
and foliage to foliage, with skin protection. The whole chain is baked into a 65³ LUT with a 1D input
shaper fitted to the frame, so the CPU and GPU paths share it. How it works and how consistent it is:
**[docs/MATCHING.md](docs/MATCHING.md)**.

## Build

Requirements: CMake 3.16+, a C++17 compiler.

```bash
# engine, command-line renderer and tests (any OS)
cmake -S . -B build && cmake --build build -j
./build/engine_tests
(cd build && ctest)   # engine, consistency and GPU tests

# consistency test on real frames: grab a few frames per clip, then point it at a reference still
swift tools/extract_frames.swift tests/frames 640 2 clip1.mov clip2.mp4
cmake -S . -B build -DLOOKMATCH_TEST_REF=/path/to/reference.png
./build/consistency_tests --old      # --old adds the previous matcher's numbers

# Premiere effect (Windows: Visual Studio 2022; macOS: Xcode 15+)
# Download the After Effects SDK from Adobe's developer site (free Adobe account), then:
cmake -S . -B build -DLOOKMATCH_BUILD_PLUGIN=ON -DAE_SDK_DIR=/path/to/AfterEffectsSDK/Examples
cmake --build build --config Release
```

Install the result into the shared Adobe plug-in folder, then restart Premiere:
- Windows: copy `LookMatch.aex` to `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`
- macOS: copy `LookMatch.plugin` to `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`

The effect shows up under **Effects > Video Effects > PRO PLUGS > LookMatch**.

### Version compatibility and the macOS installer

One build works with **Premiere Pro and After Effects CC 2015 through 2026**, so there is no per-version build.
- The effect uses only API that already existed in CC 2015: basic params, render, and `PF_PixelFormatSuite1`.
  It compiles against the CC 2015 SDK headers, apart from `PluginDataEntryFunction2`, which only newer hosts call.
  Older hosts read the PiPL instead.
- It is built against the newest SDK. The PiPL spec version stays at 13.x, which every host since CS6 accepts.
- The bundle is universal: x86_64 targets macOS 10.9+ (CC 2015, Intel, Rosetta) and arm64 targets macOS 11+.
- Every host version reads the shared `Common/Plug-ins/7.0/MediaCore` folder, so one install location covers all of them.

**Installer app (for users):** `installer/mac/build_app.sh /path/to/AfterEffectsSDK/Examples [/path/to/PremiereSDK/Examples]` builds
`build-installer/LookMatch-Installer.dmg`, which contains `Install LookMatch.app` (SwiftUI, macOS 11+).
The app lists the Premiere Pro / After Effects versions it finds, with their version numbers, right in the window.
It marks versions older than CC 2015, and has **Locate…** for an app installed somewhere else.
If it finds nothing, it says so on the page (no alert) and offers **Continue Without**.
It then copies the plug-in into the shared folder with the standard macOS password prompt, and writes `/Library/Logs/LookMatch-install.log`.
Pass the Premiere Pro SDK to build in GPU rendering. The dmg also carries the release panel (`.ccx`): when Premiere Pro 2025 or later is found,
the app installs the panel with Adobe's UnifiedPluginInstallerAgent and reports the result on the last page (the effect works without it).
Screenshots of each step are in `docs/installer/`.

**Pkg (scripted / MDM installs, and Macs older than macOS 11):** `installer/mac/build_pkg.sh /path/to/AfterEffectsSDK/Examples`
builds `build-installer/LookMatch-Installer.pkg`. It shows no alerts and only refuses to run on macOS older than 10.9.
Its postinstall script logs the hosts it found to the same log file.

Set `CODESIGN_ID` (and `SIGN_ID` for the pkg) to your Developer ID identities for a signed release.
Otherwise the builds are ad-hoc signed and the pkg is unsigned.

## Try the engine without Premiere

```bash
./build/lookmatch-render --in frame.png --ref reference.jpg --out matched.png
./build/lookmatch-render --in frame.png --out film.png --film 1 --negative 0 --print 1 \
    --halation 0.6 --bloom 0.2 --grain 0.7 --vignette 0.3 --export-lut look.cube
./build/lookmatch-render --list-stocks
```

## LookMatch panel (Premiere Pro 2025+)

A dockable panel drives the effect. It has a reference library with drag and drop, matching of selected clips or a
whole sequence, hero-clip shot matching, the source format (from clip metadata, detected, or chosen) and how much to
normalise, each clip's analysis before and after the match (format, exposure, temperature, tint, contrast), the
texture controls, saved looks (`.lmlook`), `.cube` export and a before/after split. See
**[docs/PANEL.md](docs/PANEL.md)**.

![LookMatch panel in Premiere](docs/panel/premiere-with-panel.png)

## GPU rendering (Premiere Pro, Metal)

With the Premiere Pro SDK, the effect also registers a Premiere GPU filter (`src/premiere/LookMatchGPU.mm`):

```bash
cmake -S . -B build -DLOOKMATCH_BUILD_PLUGIN=ON -DAE_SDK_DIR=<AE SDK>/Examples \
      -DLOOKMATCH_PR_SDK_DIR="<Premiere Pro SDK>/Examples"   # any SDK from CC 2017 on
```

- **On the GPU:** colour (match, film, fade, via one baked LUT and its input shaper), grain, vignette, mix and split view,
  for footage the effect reads as Rec.709. Log footage matched through **Source Format** renders on the GPU too, as the
  match LUT decodes it.
  The Metal kernel mirrors the CPU fast path. `gpu_tests` checks GPU against CPU (max 0/255). In Premiere 26.5 the
  Program monitor differed by a mean of 0.2/255 between the two paths.
- **On the CPU:** clips with halation, bloom, softness, gate weave, flicker or a log **Footage Encoding**, and clips with a panel
  request still waiting. The GPU filter declines those, and Premiere renders them with the CPU effect.
- **Speed (M1 Max, colour + grain + vignette):** 1080p is 10.3 ms on the CPU and 0.97 ms on the GPU (11×). 4K is
  40.6 ms vs 1.8 ms (23×). Measured with `gpu_bench`.
- `LOOKMATCH_NO_GPU=1` turns the GPU path off.
- The GPU filter API returns popups 0-based, while the AE API and Effect Controls are 1-based. The code accounts for this.

## Status

| Part | State |
|---|---|
| Engine (match, film, texture) | `engine_tests` passes: identity, film neutrality, export LUT, self-match, fast path vs full path, concurrent renders, transfer curves, CCT/tint, cast and log detection |
| Source-aware match | `consistency_tests`: real frames under exposure, white balance, curve, saturation and log perturbations, CIEDE2000 against the untouched match. See [docs/MATCHING.md](docs/MATCHING.md) |
| Command-line renderer | Built and tested |
| Premiere effect | Tested in Premiere Pro 2026 (26.5) on macOS: matching, film, split view, panel link. Universal arm64 + x86_64. |
| Panel | Tested in Premiere 26.5 through its test agent (self-test 8/8 or 9/9 with a reference); `node src/panel-uxp/test/run.js` |
| Speed (M1 Max) | CPU: 1080p colour-only 9.6 ms, everything on 42 ms; 4K colour-only 37 ms, everything on 196 ms. GPU (colour path): 1080p 0.97 ms, 4K 1.8 ms |

## Next steps

1. **GPU versions of halation, bloom and softness**, so every look renders on the GPU (CUDA/OpenCL for Windows).
2. **Consistency under strong per-channel curves** (S-curves, per-channel lift/gamma/gain): see the open cases in docs/MATCHING.md.
3. **Windows build** of the effect and panel (the code is portable; untested).
4. **DaVinci Resolve OFX** build, reusing the same engine.
5. Signed and notarised installers, licensing, and per-stock tuning against real film scans.

## Sample renders

- `docs/sheet_match.png`: look match (footage, reference, result)
- `docs/sheet_neg.png`: every negative stock through Cine Print Warm, with halation, grain and vignette
- `docs/sheet_print.png`: one negative through every print stock
- `docs/texture_crop.png`: 1080p close-up of grain, halation and bloom (left: original)

Sample footage is the astronaut, coffee and rocket images from scikit-image's public sample data.
The reference grades were made with the Python prototype.

## Python prototype

`prototype/` holds the original Python look matcher (`lookmatch.py`, needs numpy and pillow). It
makes a .cube LUT straight from a reference still and a footage frame, and `test_regions.py` has
the region-matching tests. The C++ matcher in `src/core/matcher.cpp` is a port of it.
