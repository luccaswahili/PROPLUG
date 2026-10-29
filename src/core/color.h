// Colour math shared by the whole engine: camera log curves, gamuts, OKLab.
#pragma once
#include <algorithm>
#include <cmath>

namespace lm {

struct Vec3 {
    float r = 0, g = 0, b = 0;
    Vec3() = default;
    Vec3(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
    float& operator[](int i) { return (&r)[i]; }
    float operator[](int i) const { return (&r)[i]; }
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.r * s, a.g * s, a.b * s}; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline float clamp01(float x) { return std::min(1.0f, std::max(0.0f, x)); }
inline Vec3 clamp01(Vec3 v) { return {clamp01(v.r), clamp01(v.g), clamp01(v.b)}; }

struct Mat3 {
    float m[3][3];
    Vec3 operator*(Vec3 v) const {
        return {m[0][0] * v.r + m[0][1] * v.g + m[0][2] * v.b,
                m[1][0] * v.r + m[1][1] * v.g + m[1][2] * v.b,
                m[2][0] * v.r + m[2][1] * v.g + m[2][2] * v.b};
    }
};

// ---------- input encodings ----------

enum class InputSpace { Rec709 = 0, ArriLogC3, SonySLog3, PanaVLog, Count };
inline const char* inputSpaceName(InputSpace s) {
    static const char* n[] = {"Rec.709", "ARRI LogC3 / AWG3", "Sony S-Log3 / S-Gamut3.Cine", "Panasonic V-Log / V-Gamut"};
    return n[int(s)];
}

// Rec.709 OETF and its inverse (display-referred footage is treated as scene light 0..1).
inline float rec709Encode(float l) {
    l = std::max(l, 0.0f);
    return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
}
inline float rec709Decode(float v) {
    return v < 0.081f ? v / 4.5f : std::pow((std::max(v, 0.0f) + 0.099f) / 1.099f, 1.0f / 0.45f);
}

inline float logC3Decode(float t) {  // EI 800
    const float cut = 0.010591f, a = 5.555556f, b = 0.052272f, c = 0.247190f, d = 0.385537f, e = 5.367655f, f = 0.092809f;
    return t > e * cut + f ? (std::pow(10.0f, (t - d) / c) - b) / a : (t - f) / e;
}
inline float logC3Encode(float x) {
    const float cut = 0.010591f, a = 5.555556f, b = 0.052272f, c = 0.247190f, d = 0.385537f, e = 5.367655f, f = 0.092809f;
    return x > cut ? c * std::log10(a * x + b) + d : e * x + f;
}
inline float sLog3Decode(float t) {
    return t >= 171.2102946929f / 1023.0f ? std::pow(10.0f, (t * 1023.0f - 420.0f) / 261.5f) * 0.19f - 0.01f
                                          : (t * 1023.0f - 95.0f) * 0.01125f / (171.2102946929f - 95.0f);
}
inline float sLog3Encode(float x) {
    return x >= 0.01125f ? (420.0f + std::log10((x + 0.01f) / 0.19f) * 261.5f) / 1023.0f
                         : (x * (171.2102946929f - 95.0f) / 0.01125f + 95.0f) / 1023.0f;
}
inline float vLogDecode(float t) {
    const float b = 0.00873f, c = 0.241514f, d = 0.598206f;
    return t < 0.181f ? (t - 0.125f) / 5.6f : std::pow(10.0f, (t - d) / c) - b;
}
inline float vLogEncode(float x) {
    const float b = 0.00873f, c = 0.241514f, d = 0.598206f;
    return x < 0.01f ? 5.6f * x + 0.125f : c * std::log10(x + b) + d;
}

// Camera gamut -> Rec.709 primaries (linear).
inline const Mat3& gamutToRec709(InputSpace s) {
    static const Mat3 I{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    static const Mat3 AWG3{{{1.617523f, -0.537287f, -0.080237f}, {-0.070573f, 1.334613f, -0.264040f}, {-0.021102f, -0.226954f, 1.248056f}}};
    static const Mat3 SG3C{{{1.6269474f, -0.5401090f, -0.0868384f}, {-0.1785155f, 1.4179409f, -0.2394254f}, {-0.0444361f, -0.1959200f, 1.2403561f}}};
    static const Mat3 VG{{{1.806576f, -0.695697f, -0.110879f}, {-0.170090f, 1.305955f, -0.135865f}, {-0.025206f, -0.154468f, 1.179674f}}};
    switch (s) {
        case InputSpace::ArriLogC3: return AWG3;
        case InputSpace::SonySLog3: return SG3C;
        case InputSpace::PanaVLog: return VG;
        default: return I;
    }
}

// Code value -> scene linear in camera primaries, and back (used to re-encode after spatial passes).
inline float decodeChannel(InputSpace s, float v) {
    switch (s) {
        case InputSpace::ArriLogC3: return logC3Decode(v);
        case InputSpace::SonySLog3: return sLog3Decode(v);
        case InputSpace::PanaVLog: return vLogDecode(v);
        default: return rec709Decode(v);
    }
}
inline float encodeChannel(InputSpace s, float l) {
    switch (s) {
        case InputSpace::ArriLogC3: return logC3Encode(l);
        case InputSpace::SonySLog3: return sLog3Encode(l);
        case InputSpace::PanaVLog: return vLogEncode(l);
        default: return rec709Encode(l);
    }
}
inline Vec3 decode(InputSpace s, Vec3 v) { return {decodeChannel(s, v.r), decodeChannel(s, v.g), decodeChannel(s, v.b)}; }
inline Vec3 encode(InputSpace s, Vec3 l) { return {encodeChannel(s, l.r), encodeChannel(s, l.g), encodeChannel(s, l.b)}; }

// ---------- display (sRGB-ish Rec.709 gamma) <-> OKLab ----------

inline float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float linearToSrgb(float c) {
    c = clamp01(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline Vec3 rgbToOklab(Vec3 rgb) {
    float r = srgbToLinear(rgb.r), g = srgbToLinear(rgb.g), b = srgbToLinear(rgb.b);
    float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}
// ACES Reference Gamut Compression (Academy RGC specification, S-2021-001): out-of-gamut colours are
// pulled toward the achromatic axis smoothly instead of being clipped channel by channel, which shifts
// hue and leaves flat blotches. Per channel, the distance d = (A - c) / |A| from the achromatic A =
// max(R, G, B) is compressed beyond a threshold t so that the limit l maps to 1 (the gamut boundary).
inline Vec3 gamutCompress(Vec3 c) {
    const float A = std::max({c.r, c.g, c.b});
    if (A <= 0) return c;
    constexpr float t[3] = {0.815f, 0.803f, 0.880f}, lim[3] = {1.147f, 1.264f, 1.312f}, p = 1.2f;
    Vec3 o = c;
    for (int i = 0; i < 3; ++i) {
        const float d = (A - c[i]) / A;
        if (d < t[i]) continue;
        const float s = (lim[i] - t[i]) / std::pow(std::pow((1 - t[i]) / (lim[i] - t[i]), -p) - 1, 1 / p);
        const float x = (d - t[i]) / s;
        const float dc = t[i] + s * x / std::pow(1 + std::pow(x, p), 1 / p);
        o[i] = A - dc * A;
    }
    return o;
}

inline Vec3 oklabToRgb(Vec3 lab) {
    float l = lab.r + 0.3963377774f * lab.g + 0.2158037573f * lab.b;
    float m = lab.r - 0.1055613458f * lab.g - 0.0638541728f * lab.b;
    float s = lab.r - 0.0894841775f * lab.g - 1.2914855480f * lab.b;
    l = l * l * l; m = m * m * m; s = s * s * s;
    return {linearToSrgb(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s),
            linearToSrgb(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
            linearToSrgb(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s)};
}
// The same, with out-of-gamut colours gamut-compressed rather than clipped.
inline Vec3 oklabToRgbCompressed(Vec3 lab) {
    float l = lab.r + 0.3963377774f * lab.g + 0.2158037573f * lab.b;
    float m = lab.r - 0.1055613458f * lab.g - 0.0638541728f * lab.b;
    float s = lab.r - 0.0894841775f * lab.g - 1.2914855480f * lab.b;
    l = l * l * l; m = m * m * m; s = s * s * s;
    const Vec3 c = gamutCompress({4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                                  -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                                  -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s});
    return {linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b)};
}

inline float luma709(Vec3 c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

}  // namespace lm
