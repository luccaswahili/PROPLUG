# LookMatch: steal the look of a still, apply it to footage

## Use it today (no plugin install needed)
1. In Premiere, park on a representative frame and use **File > Export > Frame** (PNG).
2. Run: `pip install numpy pillow` then
   `python lookmatch.py reference.jpg frame1.png [frame2.png ...] -o mylook.cube --preview check.png`
   Several frames from across the clip give a steadier match. The script prints which colour
   regions it found in both images and matched.
3. In Premiere: **Lumetri Color > Creative > Look > Browse** and pick `mylook.cube`.
   Use Lumetri's *Intensity* slider, or `--strength 0.6`, to dial it back.

Do your normalisation (exposure, white balance, Log to Rec.709 conversion) **before** the
look in Basic Correction, and export the frame from that state, so the match only has to move style.

## How the match works (all in OKLab)
| Stage | What it matches | Method |
|---|---|---|
| Regions | like with like | Every colour gets soft membership in 8 regions: neutral, red, skin, yellow, green, cyan, blue, magenta |
| Tone | contrast, black/white levels, fade | Monotonic lightness curve from histogram matching, smoothed |
| Color | saturation, dominant hue axis, colour spread | Monge-Kantorovich transfer of the (a,b) distribution |
| Region match | skin to skin, sky to sky, foliage to foliage | Per-region offsets for every region that covers 3%+ of both images |
| Split toning | different tints in shadows / mids / highlights | Per-band chroma offsets with soft weights |
| Skin protect | natural faces | Skin hue held within 8 degrees, chroma within -20%/+25% |
| Delivery | anything that reads a LUT | Baked to a 33-point .cube (17/65 optional) |

Global statistics come only from regions both images contain, each counted equally, so a
reference that is 70% sky no longer pushes a face shot blue.

### Controls
| Flag | Default | Effect |
|---|---|---|
| `--strength` | 1.0 | Overall amount, 0 to 1 |
| `--region-strength` | 1.0 | How hard each region is pulled toward its counterpart (0 = off) |
| `--skin-protect` | auto | 0 = skin follows the look, 1 = skin hue held. Auto is 0.3, or 0.8 when the reference has no skin |
| `--balance` | 0.5 | Fallback content balancing when the images share few regions |
| `--size` | 33 | LUT grid (17, 33, 65) |

### Test results (`test_regions.py`, synthetic scenes, 8-bit mean error vs the ideal grade)
| Case | Untouched | Old engine | New engine |
|---|---|---|---|
| Sky-heavy reference on a face shot | 15.6 | 38.3 (face turned grey-blue) | 19.2 (skin hue within 1 degree of ideal) |
| Similar content | 15.6 | 9.7 | 5.5 |
| Foliage-heavy reference on a city wall | 14.2 | 23.6 | 14.5 (skin error 21.0 → 4.7) |

When the reference and the footage share very little (cases 1 and 3), the new engine stops doing
damage but can't do much better than leaving the frame alone. That's expected: there is nothing
in common to learn from. See `samples/region_tests.png`.

**Known limits:** regions are colour-based, not object-based (a beige wall reads as skin). Hard
clipping at the gamut edge can cause kinks on very saturated colours; soft gamut compression is
a candidate next step.

## Plan for the full Premiere plugin
- **UXP panel** (JavaScript, Premiere 25+): drop a reference still, grab the current frame,
  sliders for Strength / Tone / Color / Split-tone / Skin protect, live before-after, one click
  writes the LUT and applies it to the selected clips' Lumetri. The engine above ports to JS/WASM.
- **Optional GPU effect** (C++ Premiere SDK, Metal/CUDA/OpenCL): a native "LookMatch" effect
  holding the LUT plus per-clip strength and keyframable controls, for when Lumetri is not enough.
- Later: object-aware masks (face detection) instead of colour regions, soft gamut compression, look library.
