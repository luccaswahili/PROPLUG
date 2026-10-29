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

### Installers for users

`installer/` builds a Mac `.pkg` (`installer/macos/build_pkg.sh`) and a Windows setup (`installer/windows/LookMatch.iss`, Inno Setup 6).
Both install one plug-in into Adobe's shared MediaCore folder, which every Premiere Pro and After Effects
version reads. They detect the installed versions and warn about any older than the supported floor.
See `installer/versions.md`.

## Try the engine without Premiere

```bash
./build/lookmatch-render --in frame.png --ref reference.jpg --out matched.png
./build/lookmatch-render --in frame.png --out film.png --film 1 --negative 0 --print 1 \
    --halation 0.6 --bloom 0.2 --grain 0.7 --vignette 0.3 --export-lut look.cube
./build/lookmatch-render --list-stocks
```

## Status

| Part | State |
|---|---|
| Engine (match, film, texture) | Built and tested here: `engine_tests` passes. With everything off the output is pixel-identical. |
| Command-line renderer | Built and tested here |
| Premiere effect wrapper | Builds on macOS (Xcode 27, AE SDK 27.0) as a universal arm64 + x86_64 bundle. Not yet run inside Premiere. |
| Speed | CPU only for now. 1080p with every effect on takes ≈ 0.17 s/frame on 4 cores. Colour-only takes a few ms. Real-time playback needs the GPU path (next). |

## Next steps

1. **First build in Premiere** on your machine. Fix SDK compile issues and check the pixel formats (BGRA 8-bit and 32f).
2. **GPU render path** (Premiere GPU effect API with Metal / CUDA / OpenCL kernels) for real-time 4K playback. The engine is already split into a LUT plus a few blur and noise passes, which map straight onto GPU kernels.
3. **Editor window** with a reference drop zone, before/after split and stock thumbnails, like the QG UI.
4. **DaVinci Resolve OFX** build, reusing the same engine.
5. Installer, licensing, and per-stock tuning against real film scans.

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
