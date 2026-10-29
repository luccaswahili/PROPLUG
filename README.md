# LookMatch (native)

A native effect for Premiere Pro (and After Effects). It matches the look of any reference still
and finishes the shot with film print emulation, halation, bloom and grain. The C++ engine renders
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

The look match is the region-aware matcher from the Python prototype, ported to C++. It matches
skin to skin, sky to sky and foliage to foliage, with skin protection.

## Build

Requirements: CMake 3.16+, a C++17 compiler.

```bash
# engine, command-line renderer and tests (any OS)
cmake -S . -B build && cmake --build build -j
./build/engine_tests

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

`installer/mac/build_pkg.sh /path/to/AfterEffectsSDK/Examples` builds `build-installer/LookMatch-Installer.pkg`.
The installer lists the Premiere Pro / After Effects versions it finds and installs into the shared folder.
It also writes the hosts it found to `/Library/Logs/LookMatch-install.log`.
Set `CODESIGN_ID` and `SIGN_ID` to your Developer ID identities for a signed release; otherwise the build is ad-hoc signed and the pkg is unsigned.

## Try the engine without Premiere

```bash
./build/lookmatch-render --in frame.png --ref reference.jpg --out matched.png
./build/lookmatch-render --in frame.png --out film.png --film 1 --negative 0 --print 1 \
    --halation 0.6 --bloom 0.2 --grain 0.7 --vignette 0.3 --export-lut look.cube
./build/lookmatch-render --list-stocks
```

## LookMatch panel (Premiere Pro 2025+)

A dockable panel drives the effect. It has a reference library with drag and drop, matching of selected clips or a
whole sequence, hero-clip shot matching, 8 film looks plus every stock combination with previews rendered from your
own shot, all film controls, saved looks (`.lmlook`), `.cube` export and a before/after split. See
**[docs/PANEL.md](docs/PANEL.md)**.

![LookMatch panel in Premiere](docs/panel/premiere-with-panel.png)

## Status

| Part | State |
|---|---|
| Engine (match, film, texture) | `engine_tests` passes: identity, film neutrality, export LUT, self-match, fast path vs full path, concurrent renders |
| Command-line renderer | Built and tested |
| Premiere effect | Tested in Premiere Pro 2026 (26.5) on macOS: matching, film, split view, panel link. Universal arm64 + x86_64. |
| Panel | Tested in Premiere 26.5 through its test agent (self-test 8/8 or 9/9 with a reference); `node src/panel-uxp/test/run.js` |
| Speed (CPU, M1 Max) | 1080p colour-only 9.6 ms, everything on 42 ms; 4K colour-only 37 ms, everything on 196 ms |

## Next steps

1. **GPU render path** (Premiere GPU effect API with Metal / CUDA kernels) for real-time 4K with halation, bloom and grain.
2. **Windows build** of the effect and panel (the code is portable; untested).
3. **DaVinci Resolve OFX** build, reusing the same engine.
4. Signed and notarised installers, licensing, and per-stock tuning against real film scans.

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
