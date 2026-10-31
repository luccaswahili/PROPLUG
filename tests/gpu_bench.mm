// GPU kernel vs CPU fast path timing at 1080p and 4K (colour + grain + vignette).
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>
#include "engine.h"
#include "../src/premiere/LookMatchKernel.h"
using namespace lm;
int main() {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kLookMatchMetalSource] options:nil error:&err];
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lookmatch_f32"] error:&err];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        printf("GPU: %s\n", dev.name.UTF8String);
        for (auto [w, h] : {std::pair{1920, 1080}, std::pair{3840, 2160}, std::pair{8192, 4320}}) {
            Params p; p.filmStrength = 0.9f; p.grain = 0.3f; p.vignette = 0.2f;
            Engine e; e.setParams(p);
            auto lutM = std::make_shared<Lut3D>(33);
            lutM->fill([](Vec3 c) { return c; });
            e.setMatchLut(lutM);  // a match on: skin smoothing runs
            std::vector<float> src(size_t(w) * h * 4), dst(src.size());
            for (size_t i = 0; i < src.size(); i += 4) { src[i] = 0.45f; src[i + 1] = 0.38f; src[i + 2] = 0.34f; src[i + 3] = 1; }  // all skin: the worst case
            e.render({src.data(), w, h, w * 4}, {dst.data(), w, h, w * 4}, 0);
            auto t0 = std::chrono::steady_clock::now();
            for (int f = 1; f <= 20; ++f) e.render({src.data(), w, h, w * 4}, {dst.data(), w, h, w * 4}, f);
            double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
            FinishParams fp; e.prepareDirect(w, h, 1, fp);
            const Lut3D& lut = e.directLut();
            id<MTLBuffer> io = [dev newBufferWithLength:src.size() * 4 options:MTLResourceStorageModePrivate];
            id<MTLBuffer> srcCopy = [dev newBufferWithLength:src.size() * 4 options:MTLResourceStorageModePrivate];
            id<MTLBuffer> lb = [dev newBufferWithBytes:lut.data() length:size_t(lut.size()) * lut.size() * lut.size() * 12 options:MTLResourceStorageModeShared];
            LookMatchGPUParams gp = makeGPUParams(fp, w, w, h, lut.size(), false, 0.5f, 0);
            auto run = [&] {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];  // as the GPU filter: deband reads a copy
                [blit copyFromBuffer:io sourceOffset:0 toBuffer:srcCopy destinationOffset:0 size:src.size() * 4];
                [blit endEncoding];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:pso]; [enc setBuffer:io offset:0 atIndex:0];
                [enc setBytes:&gp length:sizeof gp atIndex:1]; [enc setBuffer:lb offset:0 atIndex:2];
                [enc setBuffer:srcCopy offset:0 atIndex:3];
                [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(pso.threadExecutionWidth, 16, 1)];
                [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
            };
            run();
            t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; ++i) run();
            double gpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
            printf("%dx%d  CPU %.1f ms  GPU %.2f ms (incl. submit+wait)  %.0fx\n", w, h, cpu, gpu, cpu / gpu);
        }
    }
}
