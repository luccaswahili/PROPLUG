// Metal kernel for LookMatch's GPU render (Premiere GPU filter). Compiled at runtime from this string.
// It mirrors the CPU fast path in src/core/engine.cpp (Engine::finish with the baked direct LUT)
// and applySplit in LookMatch.cpp, so GPU and CPU frames match (tests/gpu_tests.mm).
#pragma once
#include <cmath>
#include "engine.h"

// Must match the `Params` struct in the kernel below.
struct LookMatchGPUParams {
    int pitch;  // pixels per row
    int width, height;
    int lutSize;
    float grainAmp, cell, grainColor;
    float vignette, mix;
    float cx, cy, invDiag;
    int frame;
    int split;
    float splitX, splitNx, splitNy, splitCy, splitLine;
    int shaperSize;  // input shaper entries stored after the LUT in the same buffer (0 = none)
    int debandRadius;  // lm::DebandParams; the kernel reads the source from a copy of the frame
    float debandThreshold, debandDither;
    int debandNearRadius;
    float debandCoring;
    int skinRadius;  // lm::SkinSmoothParams
};

// Kernel parameters for one frame from the engine's fast-path constants and the split settings
// (same geometry as applySplit in LookMatch.cpp).
inline LookMatchGPUParams makeGPUParams(const lm::FinishParams& f, int pitch, int w, int h, int lutSize, bool split,
                                        float splitPos, float splitAngleDeg) {
    LookMatchGPUParams p{};
    p.pitch = pitch;
    p.width = w;
    p.height = h;
    p.lutSize = lutSize;
    p.grainAmp = f.grainAmp;
    p.cell = f.cell;
    p.grainColor = f.grainColor;
    p.vignette = f.vignette;
    p.mix = f.mix;
    p.cx = f.cx;
    p.cy = f.cy;
    p.invDiag = f.invDiag;
    p.frame = f.frame;
    p.split = split ? 1 : 0;
    const float a = splitAngleDeg * 3.14159265f / 180.0f;
    p.splitX = splitPos * w;
    p.splitNx = std::cos(a);
    p.splitNy = std::sin(a);
    p.splitCy = 0.5f * h;
    p.splitLine = splitPos <= 0.0f || splitPos >= 1.0f ? 0.0f : std::fmax(1.0f, h / 540.0f);  // no line at 0% / 100% (A/B)
    p.shaperSize = 0;
    p.debandRadius = f.deband.radius;
    p.debandThreshold = f.deband.threshold;
    p.debandDither = f.deband.dither;
    p.debandNearRadius = f.deband.nearRadius;
    p.debandCoring = f.deband.coring;
    p.skinRadius = f.skin.radius;
    return p;
}

static const char* kLookMatchMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct Params {
    int pitch;
    int width, height;
    int lutSize;
    float grainAmp, cell, grainColor;
    float vignette, mix;
    float cx, cy, invDiag;
    int frame;
    int split;
    float splitX, splitNx, splitNy, splitCy, splitLine;
    int shaperSize;
    int debandRadius;
    float debandThreshold, debandDither;
    int debandNearRadius;
    float debandCoring;
    int skinRadius;  // lm::SkinSmoothParams
};

// --- same hash and noise as the CPU engine ---
static uint hashu(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static uint hash4(int x, int y, int f, int c) {
    return hashu(uint(x) * 0x9E3779B1u ^ hashu(uint(y) * 0x85EBCA77u ^ hashu(uint(f) * 0xC2B2AE3Du ^ uint(c) * 0x27D4EB2Fu)));
}
static float noise(int x, int y, int f, int c) {
    uint h = hash4(x, y, f, c);
    return float((h & 0xFFFFu) + (h >> 16)) * (1.0f / 65535.0f) * 2.449f - 2.449f;
}
static float smooth01(float t) { return t * t * (3.0f - 2.0f * t); }
static float smoothstepf(float a, float b, float v) { return smooth01(clamp((v - a) / (b - a), 0.0f, 1.0f)); }

// --- deband, as lm::debandAt (same hash, same sample order); src is BGRA ---
template <typename T>
static float3 rgbAt(device const T* src, constant Params& p, int x, int y) {
    float4 v = float4(src[uint(y) * uint(p.pitch) + uint(x)]);
    return float3(v.z, v.y, v.x);
}
constant int kDebandPairs = 8, kDebandNearPairs = 6;  // = lm::kDebandPairs, lm::kDebandNearPairs
template <typename T>
static float3 deband(device const T* src, constant Params& p, int x, int y) {
    float3 c = rgbAt(src, p, x, y);
    if (p.debandRadius <= 0) return c;
    uint span = uint(2 * p.debandRadius + 1), nearSpan = uint(2 * p.debandNearRadius + 1);
    float3 sum = c;
    float wsum = 1.0f;
    for (int k = 0; k < kDebandPairs + kDebandNearPairs; ++k) {
        uint h = hash4(x, y, 0x5EED, k);
        uint sp = k < kDebandPairs ? span : nearSpan;
        int rad = k < kDebandPairs ? p.debandRadius : p.debandNearRadius;
        int dx = int((h & 0xFFFFu) % sp) - rad, dy = int((h >> 16) % sp) - rad;
        float3 qa = rgbAt(src, p, clamp(x + dx, 0, p.width - 1), clamp(y + dy, 0, p.height - 1));
        float3 qb = rgbAt(src, p, clamp(x - dx, 0, p.width - 1), clamp(y - dy, 0, p.height - 1));
        float3 m = abs(0.5f * (qa + qb) - c), da = abs(qa - c), db = abs(qb - c);
        float mid = max(m.x, max(m.y, m.z));
        float side = 0.5f * max(max(da.x, max(da.y, da.z)), max(db.x, max(db.y, db.z)));
        float t = k < kDebandPairs ? p.debandThreshold : 0.5f * p.debandThreshold;
        float w = 1.0f - smoothstepf(0.5f * t, t, max(mid, side));
        sum += (qa + qb) * w;
        wsum += 2.0f * w;
    }
    float3 o = sum * (1.0f / wsum);
    float dev = ((c.x + c.y + c.z) - (o.x + o.y + o.z)) / 3.0f;
    float back = dev > p.debandCoring ? dev - p.debandCoring : dev < -p.debandCoring ? dev + p.debandCoring : 0.0f;
    float amount = (wsum - 1.0f) / float(2 * (kDebandPairs + kDebandNearPairs));
    uint h = hash4(x, y, 0x5EED, 64);
    float dith = (float((h & 0xFFFFu) + (h >> 16)) * (1.0f / 65535.0f) - 1.0f) * p.debandDither * amount;
    return o + float3(back + dith);
}

// --- skin colour smoothing, as lm::skinSmoothAt (same mask, same samples) ---
constant int kSkinSamples = 24;  // = lm::kSkinSamples
constant float2 kSkinDisc[kSkinSamples] = {
    {0.1443f, 0.0000f}, {-0.1843f, 0.1689f}, {0.0282f, -0.3215f}, {0.2324f, 0.3031f},
    {-0.4264f, -0.0754f}, {0.4039f, -0.2569f}, {-0.1351f, 0.5026f}, {-0.2577f, -0.4961f},
    {0.5590f, 0.2041f}, {-0.5816f, 0.2401f}, {0.2803f, -0.5991f}, {0.2072f, 0.6605f},
    {-0.6244f, -0.3619f}, {0.7325f, -0.1610f}, {-0.4470f, 0.6359f}, {-0.1033f, -0.7970f},
    {0.6340f, 0.5343f}, {-0.8532f, 0.0353f}, {0.6223f, -0.6193f}, {-0.0416f, 0.9004f},
    {-0.5922f, -0.7096f}, {0.9380f, 0.1262f}, {-0.7948f, 0.5530f}, {0.2172f, -0.9654f},
};
static float step01(float a, float b, float v) { float t = clamp((v - a) / (b - a), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
static float skinMask(float3 c) {
    float d1 = c.x - c.y, d2 = c.y - c.z, sat = c.x - c.z;
    if (d1 <= 0 || d2 <= 0) return 0;
    float hue = d2 / (d1 + d2), y = (c.x + c.y + c.z) / 3;
    return step01(0.22f, 0.29f, hue) * (1 - step01(0.50f, 0.58f, hue)) * step01(0.012f, 0.03f, sat) * (1 - step01(0.16f, 0.22f, sat)) *
           step01(0.10f, 0.18f, y) * (1 - step01(0.90f, 0.97f, y));
}
template <typename T>
static float3 skinSmooth(device const T* src, constant Params& p, int x, int y, float3 base) {
    if (p.skinRadius <= 0) return float3(0);
    float3 c = rgbAt(src, p, x, y);
    float m = skinMask(c);
    if (m <= 0) return float3(0);
    float yc = (c.x + c.y + c.z) / 3;
    float3 vc = c - float3(yc), sum = vc;
    float wsum = 1;
    for (int k = 0; k < kSkinSamples; ++k) {
        int xx = clamp(x + int(floor(kSkinDisc[k].x * p.skinRadius + 0.5f)), 0, p.width - 1);
        int yy = clamp(y + int(floor(kSkinDisc[k].y * p.skinRadius + 0.5f)), 0, p.height - 1);
        float3 q = rgbAt(src, p, xx, yy);
        float yq = (q.x + q.y + q.z) / 3;
        float3 vq = q - float3(yq), dv = abs(vq - vc);
        float w = skinMask(q) * (1 - step01(0.03f, 0.08f, fabs(yq - yc))) * (1 - step01(0.012f, 0.03f, max(dv.x, max(dv.y, dv.z))));
        sum += vq * w;
        wsum += w;
    }
    return (sum * (1.0f / wsum) - (base - float3((base.x + base.y + base.z) / 3))) * m;
}

template <typename T>
static float3 sourceAt(device const T* src, constant Params& p, int x, int y) {
    float3 c = deband(src, p, x, y);
    return c + skinSmooth(src, p, x, y, c);
}

// --- tetrahedral 3D LUT, red fastest, 3 floats per entry ---
static float3 lutAt(device const float* lut, int n, int r, int g, int b) {
    int i = ((b * n + g) * n + r) * 3;
    return float3(lut[i], lut[i + 1], lut[i + 2]);
}
static float3 applyLut(device const float* lut, int size, float3 c) {
    const float n = float(size - 1);
    float fr = clamp(c.x, 0.0f, 1.0f) * n, fg = clamp(c.y, 0.0f, 1.0f) * n, fb = clamp(c.z, 0.0f, 1.0f) * n;
    int r0 = min(int(fr), size - 2), g0 = min(int(fg), size - 2), b0 = min(int(fb), size - 2);
    float dr = fr - r0, dg = fg - g0, db = fb - b0;
    float3 c000 = lutAt(lut, size, r0, g0, b0), c111 = lutAt(lut, size, r0 + 1, g0 + 1, b0 + 1);
    if (dr > dg) {
        if (dg > db) {
            return c000 * (1 - dr) + lutAt(lut, size, r0 + 1, g0, b0) * (dr - dg) + lutAt(lut, size, r0 + 1, g0 + 1, b0) * (dg - db) + c111 * db;
        } else if (dr > db) {
            return c000 * (1 - dr) + lutAt(lut, size, r0 + 1, g0, b0) * (dr - db) + lutAt(lut, size, r0 + 1, g0, b0 + 1) * (db - dg) + c111 * dg;
        } else {
            return c000 * (1 - db) + lutAt(lut, size, r0, g0, b0 + 1) * (db - dr) + lutAt(lut, size, r0 + 1, g0, b0 + 1) * (dr - dg) + c111 * dg;
        }
    } else {
        if (db > dg) {
            return c000 * (1 - db) + lutAt(lut, size, r0, g0, b0 + 1) * (db - dg) + lutAt(lut, size, r0, g0 + 1, b0 + 1) * (dg - dr) + c111 * dr;
        } else if (db > dr) {
            return c000 * (1 - dg) + lutAt(lut, size, r0, g0 + 1, b0) * (dg - db) + lutAt(lut, size, r0, g0 + 1, b0 + 1) * (db - dr) + c111 * dr;
        } else {
            return c000 * (1 - dg) + lutAt(lut, size, r0, g0 + 1, b0) * (dg - dr) + lutAt(lut, size, r0 + 1, g0 + 1, b0) * (dr - db) + c111 * db;
        }
    }
}

// 1D input shaper (as lm::Lut3D::shape), stored after the LUT entries
static float shape(device const float* s, int n, float v) {
    float t = clamp(v, 0.0f, 1.0f) * float(n - 1);
    int i = min(int(t), n - 2);
    return s[i] + (t - float(i)) * (s[i + 1] - s[i]);
}

// src: input pixel as RGB, lin: the (debanded) value the grade reads; returns the graded RGB.
static float3 shade(float3 src, float3 lin, uint2 xy, constant Params& p, device const float* lut) {
    float3 sc = lin;
    if (p.shaperSize > 1) {
        device const float* s = lut + p.lutSize * p.lutSize * p.lutSize * 3;
        sc = float3(shape(s, p.shaperSize, lin.x), shape(s, p.shaperSize, lin.y), shape(s, p.shaperSize, lin.z));
    }
    float3 c = applyLut(lut, p.lutSize, sc);
    int x = int(xy.x), y = int(xy.y);
    if (p.grainAmp > 0) {
        float gx = float(x) / p.cell, gy = float(y) / p.cell;
        int ix = int(floor(gx)), iy = int(floor(gy));
        float fx = smooth01(gx - ix), fy = smooth01(gy - iy);
        float vn[4];
        for (int ch = 0; ch < 4; ++ch) {
            float a = noise(ix, iy, p.frame, ch), b = noise(ix + 1, iy, p.frame, ch);
            float cc = noise(ix, iy + 1, p.frame, ch), d = noise(ix + 1, iy + 1, p.frame, ch);
            vn[ch] = (a + (b - a) * fx) + ((cc + (d - cc) * fx) - (a + (b - a) * fx)) * fy;
        }
        float y709 = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        float env = 0.15f + 1.7f * sqrt(max(0.0f, y709 * (1 - y709)));
        for (int ch = 0; ch < 3; ++ch) c[ch] += (vn[3] + p.grainColor * (vn[ch] - vn[3])) * p.grainAmp * env;
    }
    if (p.vignette > 0) {
        float r = length(float2(float(x) - p.cx, float(y) - p.cy)) * p.invDiag;
        float v = smooth01(clamp((r - 0.35f) / 0.65f, 0.0f, 1.0f));
        c = c * (1 - p.vignette * 0.8f * v);
    }
    float3 out = src + (clamp(c, 0.0f, 1.0f) - src) * p.mix;
    if (p.split != 0) {
        float d = (float(x) + 0.5f - p.splitX) * p.splitNx + (float(y) + 0.5f - p.splitCy) * p.splitNy;
        if (fabs(d) < p.splitLine) out = float3(0.9f);
        else if (d < 0) out = src;
    }
    return out;
}

// io: the frame, graded in place; src: an unchanged copy of it (deband reads neighbours)
kernel void lookmatch_f32(device float4* io [[buffer(0)]], constant Params& p [[buffer(1)]],
                          device const float* lut [[buffer(2)]], device const float4* src [[buffer(3)]],
                          uint2 xy [[thread_position_in_grid]]) {
    if (int(xy.x) >= p.width || int(xy.y) >= p.height) return;
    uint i = xy.y * uint(p.pitch) + xy.x;
    float4 px = src[i];                      // BGRA
    float3 rgb = shade(float3(px.z, px.y, px.x), sourceAt(src, p, int(xy.x), int(xy.y)), xy, p, lut);
    io[i] = float4(rgb.z, rgb.y, rgb.x, px.w);
}

kernel void lookmatch_f16(device half4* io [[buffer(0)]], constant Params& p [[buffer(1)]],
                          device const float* lut [[buffer(2)]], device const half4* src [[buffer(3)]],
                          uint2 xy [[thread_position_in_grid]]) {
    if (int(xy.x) >= p.width || int(xy.y) >= p.height) return;
    uint i = xy.y * uint(p.pitch) + xy.x;
    float4 px = float4(src[i]);
    float3 rgb = shade(float3(px.z, px.y, px.x), sourceAt(src, p, int(xy.x), int(xy.y)), xy, p, lut);
    io[i] = half4(half(rgb.z), half(rgb.y), half(rgb.x), half(px.w));
}
)METAL";
