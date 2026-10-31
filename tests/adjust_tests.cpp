// Tests for the post-match tone and colour adjustments (src/core/adjust.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "adjust.h"

using namespace lm;
static int failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

static float maxDiff(Vec3 a, Vec3 b) { return std::max({std::fabs(a.r - b.r), std::fabs(a.g - b.g), std::fabs(a.b - b.b)}); }

int main() {
    const Vec3 samples[] = {{0, 0, 0}, {1, 1, 1}, {0.5f, 0.5f, 0.5f}, {0.8f, 0.55f, 0.45f}, {0.2f, 0.4f, 0.7f}, {0.9f, 0.1f, 0.1f}};

    // all sliders at 0: exact pass-through
    for (Vec3 s : samples) CHECK(maxDiff(applyAdjust(s, Adjust{}), s) == 0, "identity");

    // every slider, both extremes: the lightness curve never turns back on itself and stays in range
    const float vals[] = {-1, -0.5f, 0.5f, 1};
    for (int which = 0; which < 4; ++which)
        for (float v : vals)
            for (float v2 : vals) {
                Adjust a;
                float* f[] = {&a.contrast, &a.shadows, &a.highlights, &a.whites};
                *f[which] = v;
                *f[(which + 1) % 4] = v2;  // combined with a neighbour too
                float prev = -1;
                for (int i = 0; i <= 1000; ++i) {
                    float L = adjustLightness(i / 1000.0f, a);
                    CHECK(L >= prev - 1e-6f && L >= 0 && L <= 1, "monotonic: slider %d = %g, %g at L %d", which, v, v2, i);
                    prev = L;
                }
            }

    // Contrast, Shadows, Highlights keep black and white where they are
    for (float v : vals) {
        Adjust a; a.contrast = v; a.shadows = v; a.highlights = v;
        CHECK(std::fabs(adjustLightness(0, a)) < 1e-6f && std::fabs(adjustLightness(1, a) - 1) < 1e-6f, "end points %g", v);
    }

    // they do what they say
    { Adjust a; a.contrast = 1; CHECK(adjustLightness(0.25f, a) < 0.25f && adjustLightness(0.75f, a) > 0.75f, "contrast"); }
    { Adjust a; a.shadows = 1; CHECK(adjustLightness(0.3f, a) > 0.33f && std::fabs(adjustLightness(0.8f, a) - 0.8f) < 1e-6f, "shadows"); }
    { Adjust a; a.highlights = -1; CHECK(adjustLightness(0.7f, a) < 0.67f && std::fabs(adjustLightness(0.3f, a) - 0.3f) < 1e-6f, "highlights"); }
    { Adjust a; a.whites = -1; CHECK(adjustLightness(1, a) < 0.92f && std::fabs(adjustLightness(0.4f, a) - 0.4f) < 1e-6f, "whites"); }

    // tone sliders leave hue alone: a skin tone keeps its OKLab hue angle
    {
        const Vec3 skin = {0.78f, 0.56f, 0.45f};
        const Vec3 l0 = rgbToOklab(skin);
        Adjust a; a.contrast = 0.6f; a.shadows = 0.5f; a.highlights = -0.5f;
        const Vec3 l1 = rgbToOklab(applyAdjust(skin, a));
        CHECK(std::fabs(std::atan2(l1.b, l1.g) - std::atan2(l0.b, l0.g)) < 0.02f, "skin hue kept");
    }

    // Exposure +1: a mid tone doubles in linear light
    {
        Adjust a; a.exposure = 1;
        const float c = rec709Encode(0.1f);
        const Vec3 o = applyAdjust({c, c, c}, a);
        CHECK(std::fabs(rec709Decode(o.g) - 0.2f) < 0.002f, "exposure +1: %g", rec709Decode(o.g));
        // and white rolls in instead of clipping: two bright tones stay apart
        const Vec3 h1 = applyAdjust({0.8f, 0.8f, 0.8f}, a), h2 = applyAdjust({0.9f, 0.9f, 0.9f}, a);
        CHECK(h2.g > h1.g + 0.01f && h2.g <= 1.0f, "exposure roll-off %g %g", h1.g, h2.g);
    }
    // ... and fades in continuously from 0
    {
        Adjust a; a.exposure = 0.001f;
        for (Vec3 s : samples) CHECK(maxDiff(applyAdjust(s, a), s) < 0.002f, "exposure continuity");
        Adjust t; t.temperature = 0.001f;
        for (Vec3 s : samples) CHECK(maxDiff(applyAdjust(s, t), s) < 0.002f, "temperature continuity");
    }
    // Exposure -1 halves
    {
        Adjust a; a.exposure = -1;
        const float c = rec709Encode(0.2f);
        CHECK(std::fabs(rec709Decode(applyAdjust({c, c, c}, a).g) - 0.1f) < 0.002f, "exposure -1");
    }

    // Temperature: warm grey goes red-ward and keeps its brightness, cool goes blue-ward
    {
        const float c = rec709Encode(0.18f);
        Adjust w; w.temperature = 0.5f;
        const Vec3 o = applyAdjust({c, c, c}, w);
        CHECK(o.r > o.g && o.g > o.b, "warm");
        const Vec3 lin = {rec709Decode(o.r), rec709Decode(o.g), rec709Decode(o.b)};
        CHECK(std::fabs(luma709(lin) - 0.18f) < 0.003f, "warm keeps luma %g", luma709(lin));
        Adjust k; k.temperature = -0.5f;
        const Vec3 q = applyAdjust({c, c, c}, k);
        CHECK(q.b > q.g && q.g > q.r, "cool");
    }

    // Saturation: -1 is grey, +1 roughly doubles chroma, neutral stays neutral
    {
        const Vec3 s = {0.7f, 0.45f, 0.35f};
        Adjust g; g.saturation = -1;
        const Vec3 o = applyAdjust(s, g);
        CHECK(std::fabs(o.r - o.g) < 0.003f && std::fabs(o.g - o.b) < 0.003f, "saturation -1 grey");
        Adjust d; d.saturation = 1;
        const Vec3 l0 = rgbToOklab(s), l1 = rgbToOklab(applyAdjust(s, d));
        const float c0 = std::hypot(l0.g, l0.b), c1 = std::hypot(l1.g, l1.b);
        CHECK(c1 > 1.8f * c0 && c1 < 2.2f * c0, "saturation +1: %g -> %g", c0, c1);
        const Vec3 n = applyAdjust({0.5f, 0.5f, 0.5f}, d);
        CHECK(maxDiff(n, {0.5f, 0.5f, 0.5f}) < 0.003f, "neutral stays neutral");
    }

    if (failures) { std::printf("%d adjust test(s) failed\n", failures); return 1; }
    std::printf("adjust tests passed\n");
    return 0;
}
