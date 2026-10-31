// GPU kernel vs CPU engine: the Metal kernel used by the Premiere GPU filter must give the same
// pixels as the CPU fast path (within 1/255). Runs on this Mac's default GPU.
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include "engine.h"
#include "matcher.h"
#include "../src/premiere/LookMatchKernel.h"

using namespace lm;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

// CPU split, as applySplit in LookMatch.cpp
static void cpuSplit(std::vector<float>& after, const std::vector<float>& before, int w, int h, float pos, float ang) {
    const float a = ang * 3.14159265f / 180.0f, nx = std::cos(a), ny = std::sin(a), cx = pos * w, cy = 0.5f * h;
    const float line = pos <= 0.0f || pos >= 1.0f ? 0.0f : std::max(1.0f, h / 540.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float d = (x + 0.5f - cx) * nx + (y + 0.5f - cy) * ny;
            size_t i = (size_t(y) * w + x) * 4;
            if (std::abs(d) < line) after[i] = after[i + 1] = after[i + 2] = 0.9f;
            else if (d < 0) for (int c = 0; c < 3; ++c) after[i + c] = before[i + c];
        }
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::printf("no Metal device: skipped\n"); return 0; }
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kLookMatchMetalSource] options:nil error:&error];
        if (!lib) { std::printf("FAIL kernel compile: %s\n", error.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lookmatch_f32"] error:&error];
        id<MTLCommandQueue> queue = [device newCommandQueue];

        const int w = 640, h = 360;
        std::vector<float> src(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float* p = &src[(size_t(y) * w + x) * 4];
                p[0] = float(x) / (w - 1); p[1] = float(y) / (h - 1); p[2] = 0.5f + 0.5f * std::sin(x * 0.05f + y * 0.03f); p[3] = 1;
                if (x < w / 2 && y < h / 2) {  // a patch of skin with blotches, for the skin smoothing
                    const float n = 0.01f * std::sin(x * 0.4f) * std::sin(y * 0.3f), l = 0.1f * float(x) / w;
                    p[0] = 0.42f + l + n; p[1] = 0.36f + l - n; p[2] = 0.33f + l - n;
                }
            }

        // a source-aware match (shaped LUT), built from the test image against a warm, contrasty variant of it
        std::shared_ptr<const Lut3D> sceneLut;
        {
            Image a, b;
            a.width = b.width = w; a.height = b.height = h;
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                Vec3 c(src[i * 4], src[i * 4 + 1], src[i * 4 + 2]);
                a.px.push_back(c * 0.8f);
                b.px.push_back(clamp01(Vec3(c.r * 1.1f, c.g * 0.95f, c.b * 0.8f)));
            }
            sceneLut = std::make_shared<Lut3D>(matchScene(b, a, SceneMatchOptions{}).lut);
        }
        struct Case { const char* name; Params p; bool split; bool scene; float pos = 0.4f; float angle = 15.0f; } cases[] = {
            {"colour only", [] { Params p; p.filmStrength = 0.8f; p.film.negative = 1; p.film.print = 1; return p; }(), false},
            {"grain + vignette + mix", [] { Params p; p.filmStrength = 0.6f; p.grain = 0.7f; p.grainColor = 0.4f; p.vignette = 0.4f; p.mix = 0.8f; return p; }(), false},
            {"exposure, white balance, fade", [] { Params p; p.exposure = 0.5f; p.temperature = 0.3f; p.tint = -0.2f; p.fade = 0.15f; p.filmStrength = 1; p.film.negative = 6; p.film.print = 4; return p; }(), false},
            {"split view", [] { Params p; p.filmStrength = 1; p.grain = 0.3f; return p; }(), true},
            {"A/B: original (split at 100%)", [] { Params p; p.filmStrength = 1; return p; }(), true, false, 1.0f, 0.0f},
            {"A/B: matched (split at 0%)", [] { Params p; p.filmStrength = 1; return p; }(), true, false, 0.0f, 0.0f},
            {"source-aware match (shaped LUT)", [] { Params p; p.grain = 0.2f; return p; }(), false, true},
            {"source-aware match, deband on skin", [] { Params p; p.deband = 1; return p; }(), false, true},
            {"source-aware match, strength 60%", [] { Params p; p.matchStrength = 0.6f; p.vignette = 0.3f; return p; }(), false, true},
        };
        for (auto& tc : cases) {
            Engine e;
            e.setParams(tc.p);
            if (tc.scene) e.setMatchLut(sceneLut);
            FinishParams fp;
            CHECK(e.prepareDirect(w, h, 7, fp), "%s: not a fast-path frame", tc.name);
            // CPU
            std::vector<float> cpu(src.size());
            e.render({src.data(), w, h, w * 4}, {cpu.data(), w, h, w * 4}, 7);
            if (tc.split) cpuSplit(cpu, src, w, h, tc.pos, tc.angle);
            if (tc.split && tc.pos >= 1) {  // A/B "original": the whole frame untouched, no line
                float d = 0;
                for (size_t i = 0; i < src.size(); ++i) if (i % 4 != 3) d = std::max(d, std::abs(cpu[i] - src[i]));
                CHECK(d < 1e-6f, "%s: the original isn't untouched (%.4f)", tc.name, d);
            }
            // GPU (BGRA float4, in place)
            std::vector<float> bgra(src.size());
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                bgra[i * 4] = src[i * 4 + 2]; bgra[i * 4 + 1] = src[i * 4 + 1]; bgra[i * 4 + 2] = src[i * 4]; bgra[i * 4 + 3] = src[i * 4 + 3];
            }
            const Lut3D& lut = e.directLut();
            id<MTLBuffer> io = [device newBufferWithBytes:bgra.data() length:bgra.size() * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> srcCopy = [device newBufferWithBytes:bgra.data() length:bgra.size() * 4 options:MTLResourceStorageModeShared];
            std::vector<float> lutData(reinterpret_cast<const float*>(lut.data()),
                                       reinterpret_cast<const float*>(lut.data()) + size_t(lut.size()) * lut.size() * lut.size() * 3);
            lutData.insert(lutData.end(), lut.shaper().begin(), lut.shaper().end());
            id<MTLBuffer> lb = [device newBufferWithBytes:lutData.data() length:lutData.size() * 4 options:MTLResourceStorageModeShared];
            LookMatchGPUParams gp = makeGPUParams(fp, w, w, h, lut.size(), tc.split, tc.pos, tc.angle);
            gp.shaperSize = int(lut.shaper().size());
            if (tc.scene) CHECK(gp.shaperSize > 0, "%s: the direct LUT should carry the match's shaper", tc.name);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:io offset:0 atIndex:0];
            [enc setBytes:&gp length:sizeof gp atIndex:1];
            [enc setBuffer:lb offset:0 atIndex:2];
            [enc setBuffer:srcCopy offset:0 atIndex:3];
            [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const float* g = (const float*)io.contents;
            double sum = 0; float worst = 0;
            for (size_t i = 0; i < size_t(w) * h; ++i)
                for (int c = 0; c < 3; ++c) {
                    float d = std::abs(g[i * 4 + (2 - c)] - cpu[i * 4 + c]);
                    sum += d; worst = std::max(worst, d);
                }
            double mean = sum / (size_t(w) * h * 3);
            std::printf("%-30s mean %.4f/255  max %.3f/255\n", tc.name, mean * 255, worst * 255);
            CHECK(mean * 255 < 0.1 && worst * 255 < 1.0, "%s: GPU differs from CPU", tc.name);
        }
    }
    std::printf(failures ? "%d check(s) failed\n" : "all GPU tests passed\n", failures);
    return failures ? 1 : 0;
}
