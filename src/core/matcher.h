// Look matcher: learns the look of a reference still relative to a frame of the footage and
// bakes it into a display-referred 3D LUT. C++ port of the Python prototype (lookmatch.py).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "lut3d.h"
#include "scene.h"

namespace lm {

struct MatchOptions {
    float regionStrength = 1.0f;  // how hard skin/sky/foliage are pulled to their counterparts
    float skinProtect = -1.0f;    // 0..1, negative = auto (0.3, or 0.8 when the reference has no skin)
    float balance = 0.5f;         // fallback content balancing when the images share few regions
    int lutSize = 33;
    bool matchTone = true;        // histogram-match lightness (off when the tone is handled elsewhere)
};

struct MatchResult {
    Lut3D lut;           // display RGB -> matched display RGB
    std::string report;  // human readable region table
};

// Both inputs are display-referred RGB in 0..1. Subsample large images before calling (~250k px is plenty).
MatchResult matchLook(const std::vector<Vec3>& reference, const std::vector<Vec3>& source, const MatchOptions& opt);

// Source-aware match. The source frame is analysed (log or display, exposure, white balance, tone
// curve), brought to a canonical state, and the reference's look is applied to that canonical
// state, so the result depends on what is in the shot rather than on how it was shot or graded.
struct SceneMatchOptions {
    MatchOptions look;   // region strength, skin protect, balance, LUT size
    int transfer = -1;   // source encoding (Transfer index), -1 = detect
    float exposure = 1;      // 0..1: how much to normalise exposure
    float whiteBalance = 1;  // 0..1: how much to neutralise the source's white balance
    float contrast = 1;      // 0..1: how much to undo the source's tone curve and saturation
    float lookStrength = 1;  // 0..1: how much of the reference's look to apply on top
};

struct SceneMatch {
    Lut3D lut;               // source code values -> matched display RGB
    std::function<Vec3(Vec3)> exact;  // the same transform, evaluated directly (tests)
    SceneAnalysis source;    // the source frame as it came in
    SceneAnalysis reference; // the reference still
    SceneAnalysis result;    // the source frame after the match
    std::string report;
};

// `source` holds the frame's code values (what the effect receives); `reference` is display RGB.
SceneMatch matchScene(const Image& reference, const Image& source, const SceneMatchOptions& opt);

}  // namespace lm
