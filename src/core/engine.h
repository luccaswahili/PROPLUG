// LookMatch render engine.
//
// Per frame:
//   1. gate weave (sub-pixel shift)          - spatial, full res
//   2. decode input -> scene linear Rec.709, exposure / white balance / flicker gain
//   3. halation, bloom, softness              - spatial, in scene linear (where light actually spreads)
//   4. colour LUT                              - neutral display transform, look match, film negative+print,
//                                                tone/colour adjustments (adjust.h), fade
//                                                baked into one 3D LUT whenever colour settings change
//   5. grain, vignette, mix                    - display space
//
// Steps 2 and 4 are pure per-pixel colour, which is what "Export LUT" bakes for Lumetri.
#pragma once
#include <memory>
#include <vector>
#include "adjust.h"
#include "film.h"
#include "lut3d.h"

namespace lm {

struct Params {
    // input
    InputSpace input = InputSpace::Rec709;
    float exposure = 0;     // stops
    float temperature = 0;  // -1 (cool) .. 1 (warm)
    float tint = 0;         // -1 (green) .. 1 (magenta)
    // look match (the LUT itself is set with Engine::setMatchLut)
    float matchStrength = 1;
    // film colour
    float filmStrength = 0;  // 0 = off, 1 = full print emulation
    FilmSettings film;
    float fade = 0;  // black lift 0..1
    // tone and colour adjustments after the match and film (adjust.h): Exposure, Contrast, Shadows,
    // Highlights, Whites, Saturation, Temperature
    Adjust adjust;
    // film texture
    float halation = 0, halationRadius = 1, halationThreshold = 0.6f;
    float bloom = 0, bloomRadius = 1, bloomThreshold = 0.5f;
    float grain = 0, grainSize = 1, grainColor = 0.3f;
    float softness = 0;
    float vignette = 0;
    float weave = 0;    // gate weave in pixels at 1080p
    float flicker = 0;  // exposure flicker 0..1
    // output
    float mix = 1;
    int lutSize = 65;
    // smooth the source's quantisation steps (compressed 8/10-bit gradients such as skies) before the
    // grade expands them into contour rings; 0 = off (the camera's own texture and grain stay), 1 = on
    float deband = 0;
    // pull skin's colour toward the skin tone around it (colour only, brightness and pores untouched); runs
    // with a match, 0..1
    float skinEven = 0.5f;
};

// Deband constants for one frame size (shared with the GPU kernel).
constexpr int kDebandPairs = 8;      // far neighbours, sampled in point-symmetric pairs
constexpr int kDebandNearPairs = 6;  // near neighbours: fine chroma noise, also on shaded skin
struct DebandParams {
    int radius = 0;         // sample radius in pixels, 0 = off
    float threshold = 0;    // code-value difference above which a neighbour is real detail
    float dither = 0;       // triangular dither amplitude (code values) where the pixel was smoothed
    int nearRadius = 1;     // radius of the near ring, pixels
    float coring = 0;       // brightness deviations below this (code values) are grain or steps, not texture
};
DebandParams debandParams(float amount, int height);
// The debanded code value of pixel (x, y): the mean of the pixel and pseudo-random point-symmetric pairs
// of neighbours (a far ring for banding, a near ring for fine noise), each pair weighted by how close its
// midpoint is to the centre (pairs across an edge or in texture don't count), with the pixel's own fine
// texture kept above a grain-sized coring level, plus a little dither, the same on all channels.
Vec3 debandAt(const float* data, int width, int height, int stride, int x, int y, const DebandParams& d);

// Skin tone evening, one frame size (shared with the GPU kernel).
constexpr int kSkinSamples = 24;  // neighbours on a golden-angle disc
struct SkinEvenParams {
    int radius = 0;  // disc radius in pixels, 0 = off
    float amount = 0;
};
SkinEvenParams skinEvenParams(float amount, int height);
// What to add to `base` (the code value of pixel (x, y), debanded if deband is on) so its colour moves toward the average of the skin around it. The
// average is of the colour alone (the pixel minus its mean of R, G, B), so brightness and texture stay, and
// only over neighbours that are skin, as bright and as coloured as the pixel: nothing crosses into lips,
// eyes, hair or a backdrop. Zero away from skin.
Vec3 skinEvenAt(const float* data, int width, int height, int stride, int x, int y, Vec3 base, const SkinEvenParams& s);

struct ImageView {  // interleaved float RGBA, rows may be padded
    float* data;
    int width, height, stride;  // stride in floats
    float* px(int x, int y) const { return data + size_t(y) * stride + size_t(x) * 4; }
};

// Constants of the final per-pixel pass (grain, vignette, mix), so a GPU kernel can reproduce the
// CPU fast path exactly.
struct FinishParams {
    float grainAmp = 0, cell = 1, grainColor = 0;
    float vignette = 0, mix = 1;
    float cx = 0, cy = 0, invDiag = 0;
    int frame = 0;
    DebandParams deband;
    SkinEvenParams skin;
};

class Engine {
public:
    Engine();
    void setParams(const Params& p);  // cheap when only spatial settings change
    void setMatchLut(std::shared_ptr<const Lut3D> lut);
    bool grades() const;  // the colour chain changes the picture (deband runs only then)
    const Params& params() const { return p_; }

    // in and out may be the same buffer. frame drives grain, weave and flicker.
    void render(const ImageView& in, const ImageView& out, long frame);

    // Neutral display version of an input pixel: what the matcher sees as "the footage".
    Vec3 neutralDisplay(Vec3 inputCode) const;
    // Colour-only transform from input code values to display, for .cube export (no shaper).
    Lut3D bakeExportLut(int size) const;

    // GPU support: if this frame can use the single-LUT fast path (Rec.709 input, no weave, flicker,
    // halation, bloom or softness), builds that LUT, fills `fp` and returns true.
    bool prepareDirect(int w, int h, long frame, FinishParams& fp);
    const Lut3D& directLut() const { return direct_; }

private:
    Vec3 inputGain() const;
    Lut3D bakeLut(int size, bool shaped) const;
    Vec3 colorFromLinear(Vec3 lin) const;  // scene linear Rec.709 -> display
    void rebuildLut();
    template <class ColourAt>
    void finish(const ImageView& in, const ImageView& out, long frame, ColourAt colourAt);

    Params p_;
    bool lutDirty_ = true;
    bool directDirty_ = true;  // direct_ needs rebuilding (colour or input gain changed)
    Lut3D direct_;             // input code -> display in one lookup, for frames with no spatial effects
    std::shared_ptr<const Lut3D> match_;
    std::unique_ptr<FilmModel> film_;
    Lut3D lut_;  // log2-shaped scene linear -> display
    std::vector<float> lin_, tmpA_, tmpB_;
};

}  // namespace lm
