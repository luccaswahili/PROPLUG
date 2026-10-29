// Look matcher: learns the look of a reference still relative to a frame of the footage and
// bakes it into a display-referred 3D LUT. C++ port of the Python prototype (lookmatch.py).
#pragma once
#include <string>
#include <vector>
#include "lut3d.h"

namespace lm {

struct MatchOptions {
    float regionStrength = 1.0f;  // how hard skin/sky/foliage are pulled to their counterparts
    float skinProtect = -1.0f;    // 0..1, negative = auto (0.3, or 0.8 when the reference has no skin)
    float balance = 0.5f;         // fallback content balancing when the images share few regions
    int lutSize = 33;
};

struct MatchResult {
    Lut3D lut;           // display RGB -> matched display RGB
    std::string report;  // human readable region table
};

// Both inputs are display-referred RGB in 0..1. Subsample large images before calling (~250k px is plenty).
MatchResult matchLook(const std::vector<Vec3>& reference, const std::vector<Vec3>& source, const MatchOptions& opt);

}  // namespace lm
