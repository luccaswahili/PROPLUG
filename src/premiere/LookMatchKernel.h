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
    p.splitLine = std::fmax(1.0f, h / 540.0f);
    p.shaperSize = 0;
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

// src: input pixel as RGB (BGRA in memory is swizzled by the caller); returns the graded RGB.
static float3 shade(float3 src, uint2 xy, constant Params& p, device const float* lut) {
    float3 sc = src;
    if (p.shaperSize > 1) {
        device const float* s = lut + p.lutSize * p.lutSize * p.lutSize * 3;
        sc = float3(shape(s, p.shaperSize, src.x), shape(s, p.shaperSize, src.y), shape(s, p.shaperSize, src.z));
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

kernel void lookmatch_f32(device float4* io [[buffer(0)]], constant Params& p [[buffer(1)]],
                          device const float* lut [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    if (int(xy.x) >= p.width || int(xy.y) >= p.height) return;
    uint i = xy.y * uint(p.pitch) + xy.x;
    float4 px = io[i];                       // BGRA
    float3 rgb = shade(float3(px.z, px.y, px.x), xy, p, lut);
    io[i] = float4(rgb.z, rgb.y, rgb.x, px.w);
}

kernel void lookmatch_f16(device half4* io [[buffer(0)]], constant Params& p [[buffer(1)]],
                          device const float* lut [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    if (int(xy.x) >= p.width || int(xy.y) >= p.height) return;
    uint i = xy.y * uint(p.pitch) + xy.x;
    float4 px = float4(io[i]);
    float3 rgb = shade(float3(px.z, px.y, px.x), xy, p, lut);
    io[i] = half4(half(rgb.z), half(rgb.y), half(rgb.x), half(px.w));
}
)METAL";
