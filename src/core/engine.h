// LookMatch render engine.
//
// Per frame:
//   1. gate weave (sub-pixel shift)          - spatial, full res
//   2. decode input -> scene linear Rec.709, exposure / white balance / flicker gain
//   3. halation, bloom, softness              - spatial, in scene linear (where light actually spreads)
//   4. colour LUT                              - neutral display transform, look match, film negative+print, fade
//                                                baked into one 3D LUT whenever colour settings change
//   5. grain, vignette, mix                    - display space
//
// Steps 2 and 4 are pure per-pixel colour, which is what "Export LUT" bakes for Lumetri.
#pragma once
#include <memory>
#include <vector>
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
};

struct ImageView {  // interleaved float RGBA, rows may be padded
    float* data;
    int width, height, stride;  // stride in floats
    float* px(int x, int y) const { return data + size_t(y) * stride + size_t(x) * 4; }
};

class Engine {
public:
    Engine();
    void setParams(const Params& p);  // cheap when only spatial settings change
    void setMatchLut(std::shared_ptr<const Lut3D> lut);
    const Params& params() const { return p_; }

    // in and out may be the same buffer. frame drives grain, weave and flicker.
    void render(const ImageView& in, const ImageView& out, long frame);

    // Neutral display version of an input pixel: what the matcher sees as "the footage".
    Vec3 neutralDisplay(Vec3 inputCode) const;
    // Colour-only transform from input code values to display, for .cube export.
    Lut3D bakeExportLut(int size) const;

private:
    Vec3 inputGain() const;
    Vec3 colorFromLinear(Vec3 lin) const;  // scene linear Rec.709 -> display
    void rebuildLut();

    Params p_;
    bool lutDirty_ = true;
    std::shared_ptr<const Lut3D> match_;
    std::unique_ptr<FilmModel> film_;
    Lut3D lut_;  // log2-shaped scene linear -> display
    std::vector<float> lin_, tmpA_, tmpB_;
};

}  // namespace lm
