// Basic tone and colour adjustments applied after the match: Exposure, Contrast, Shadows, Highlights,
// Whites, Saturation, Temperature.
//
// They run inside the colour LUT (engine.cpp colorFromLinear), so moving one rebuilds only that LUT,
// never the match, and the CPU and GPU renders stay identical.
//
// Everything is a smooth global curve, so the adjustments never add contours or crunch:
//   - Temperature and Exposure are gains in linear light (like a white balance / exposure change on
//     the camera), with a soft roll-off into white when Exposure goes up instead of a hard clip.
//   - Contrast, Shadows, Highlights and Whites move OKLab lightness only, through a curve that stays
//     monotonic at any slider setting; hue and chroma are untouched, so skin keeps its colour.
//   - Saturation scales OKLab chroma (hue stays put; out-of-gamut colours are gamut-compressed).
// With every slider at 0 the function is skipped, so an untouched clip passes through exactly.
#pragma once
#include <algorithm>
#include <cmath>
#include "color.h"

namespace lm {

struct Adjust {
    float exposure = 0;     // stops, -4 .. 4
    float contrast = 0;     // -1 .. 1
    float shadows = 0;      // -1 .. 1
    float highlights = 0;   // -1 .. 1
    float whites = 0;       // -1 .. 1
    float saturation = 0;   // -1 .. 1: chroma x (1 + 2s), so -0.5 and below is grey, 1 is triple chroma
    float temperature = 0;  // -1 (cool) .. 1 (warm)

    bool active() const {
        return exposure != 0 || contrast != 0 || shadows != 0 || highlights != 0 || whites != 0 || saturation != 0 ||
               temperature != 0;
    }
    bool operator!=(const Adjust& o) const {
        return exposure != o.exposure || contrast != o.contrast || shadows != o.shadows || highlights != o.highlights ||
               whites != o.whites || saturation != o.saturation || temperature != o.temperature;
    }
};

namespace adjust_detail {

// (4t(1-t))^2 on [a, b], 0 outside: a smooth bump (zero value and slope at both ends).
inline float bump(float x, float a, float b) {
    const float t = (x - a) / (b - a);
    if (t <= 0 || t >= 1) return 0;
    const float u = 4 * t * (1 - t);
    return u * u;
}

}  // namespace adjust_detail

// Lightness curve (OKLab L, 0 = black, 1 = white). Each step keeps the curve monotonic for any
// setting in range, so tones never cross over (no solarised patches, no new contours).
inline float adjustLightness(float L, const Adjust& a) {
    using adjust_detail::bump;
    // Each step is applied twice (strength doubled after Lucas's first test in Premiere, 2026-10-31). A curve
    // that is monotonic applied to itself is still monotonic, so doubling this way can't make tones cross.
    for (int pass = 0; pass < 2; ++pass) {
        // Contrast: an S around mid grey that leaves black and white where they are.
        // slope 1 + 0.45c at the pivot, 1 - 0.9c at the ends (>= 0.1).
        if (a.contrast != 0) {
            const float p = 0.5f;
            const float x = std::clamp(L, 0.0f, 1.0f);
            L = L + a.contrast * 0.45f * (x - p) * x * (1 - x) * 4;
        }
        // Shadows / Highlights: lift or lower the dark or bright half, black and white points kept.
        // Amplitude 0.12 keeps the slope above ~0.38.
        if (a.shadows != 0) L += 0.12f * a.shadows * bump(L, 0.0f, 0.62f);
        if (a.highlights != 0) L += 0.12f * a.highlights * bump(L, 0.38f, 1.0f);
        // Whites: moves the white end (and the top of the range with it), smoothly from L = 0.55 up.
        if (a.whites != 0) {
            const float t = std::clamp((L - 0.55f) / 0.45f, 0.0f, 1.0f);
            L += 0.1f * a.whites * t * t;
        }
        L = std::clamp(L, 0.0f, 1.0f);
    }
    return L;
}

// display Rec.709 code values in, display code values out
inline Vec3 applyAdjust(Vec3 code, const Adjust& a) {
    if (!a.active()) return code;
    Vec3 c = code;
    if (a.exposure != 0 || a.temperature != 0) {
        Vec3 lin = {rec709Decode(c.r), rec709Decode(c.g), rec709Decode(c.b)};
        // warm = more red / less blue, normalised so brightness stays the same (same as the input gain)
        Vec3 g(std::exp2(0.6f * a.temperature), 1.0f, std::exp2(-0.6f * a.temperature));
        g = g * (std::exp2(a.exposure) / luma709(g));
        lin = lin * g;
        // When a gain goes above 1, roll that channel's top into white instead of clipping it flat:
        // f(x) = k + (x - k) / (1 + b (x - k)) above the knee k, with b chosen so the brightest possible
        // value W (= the gain) lands exactly on 1. C1 at the knee, monotonic, and the identity at W = 1, so
        // it fades in smoothly as the slider moves.
        constexpr float k = 0.5f;
        for (int i = 0; i < 3; ++i) {
            const float W = g[i];
            if (W <= 1.0f || lin[i] <= k) continue;
            const float b = (W - 1) / ((1 - k) * (W - k));
            lin[i] = k + (lin[i] - k) / (1 + b * (lin[i] - k));
        }
        c = {rec709Encode(std::clamp(lin.r, 0.0f, 1.0f)), rec709Encode(std::clamp(lin.g, 0.0f, 1.0f)),
             rec709Encode(std::clamp(lin.b, 0.0f, 1.0f))};
    }
    if (a.contrast != 0 || a.shadows != 0 || a.highlights != 0 || a.whites != 0 || a.saturation != 0) {
        Vec3 lab = rgbToOklab(clamp01(c));
        lab.r = adjustLightness(lab.r, a);
        const float chroma = std::max(0.0f, 1 + 2 * a.saturation);  // +100 = 3x chroma, -50 and below = grey
        lab.g *= chroma;
        lab.b *= chroma;
        c = oklabToRgbCompressed(lab, true);
    }
    return clamp01(c);
}

}  // namespace lm
