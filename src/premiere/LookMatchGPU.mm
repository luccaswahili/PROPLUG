// LookMatch GPU render for Premiere Pro (Metal). Premiere renders third-party effects on the GPU
// through its own GPU filter API (xGPUFilterEntry, Premiere Pro SDK headers). This module handles
// the frames the CPU fast path covers: Rec.709 input, colour (match, film, fade) through one LUT,
// grain, vignette, mix and split view. Everything else declines in CreateInstance, which makes
// Premiere render that clip with the CPU effect (LookMatch.cpp) for the current parameters.
//
// Built only when LOOKMATCH_PR_SDK_DIR is set (CMake). LOOKMATCH_NO_GPU=1 turns it off at runtime.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

#include "PrSDKGPUDeviceSuite.h"
#include "PrSDKGPUFilter.h"
#include "PrSDKMemoryManagerSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKVideoSegmentSuite.h"

#include "LookMatchKernel.h"
#include "LookMatchParamIds.h"
#include "MatchData.h"
#include "PanelFiles.h"
#include "engine.h"

namespace {

// ---------- logging (same switch and file as the CPU effect) ----------

void gpuLog(const char* fmt, ...) {
    static const bool on = [] { const char* v = std::getenv("LOOKMATCH_LOG"); return v && v[0] == '1'; }();
    if (!on) return;
    static std::mutex m;
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::lock_guard<std::mutex> lock(m);
    FILE* f = std::fopen((std::string(home) + "/Library/Logs/LookMatch.log").c_str(), "a");
    if (!f) return;
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
    std::fprintf(f, "%s GPU: ", ts);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

bool gpuDisabled() {
    const char* v = std::getenv("LOOKMATCH_NO_GPU");
    return v && v[0] == '1';
}

// ---------- Metal pipelines, one set per device ----------

struct Pipelines {
    id<MTLComputePipelineState> f32 = nil, f16 = nil;
};
constexpr int kMaxDevices = 16;
Pipelines gPipelines[kMaxDevices];
std::mutex gPipelineMutex;

bool pipelinesFor(csSDK_uint32 deviceIndex, id<MTLDevice> device, Pipelines& out) {
    if (deviceIndex >= kMaxDevices) return false;
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    Pipelines& p = gPipelines[deviceIndex];
    if (!p.f32) {
        @autoreleasepool {
            NSError* error = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kLookMatchMetalSource]
                                                      options:nil
                                                        error:&error];
            if (!lib) {
                gpuLog("kernel compile failed: %s", error ? error.localizedDescription.UTF8String : "?");
                return false;
            }
            id<MTLFunction> f32 = [lib newFunctionWithName:@"lookmatch_f32"];
            id<MTLFunction> f16 = [lib newFunctionWithName:@"lookmatch_f16"];
            p.f32 = [device newComputePipelineStateWithFunction:f32 error:&error];
            p.f16 = [device newComputePipelineStateWithFunction:f16 error:&error];
            [f32 release];
            [f16 release];
            [lib release];
            if (!p.f32 || !p.f16) {
                gpuLog("pipeline creation failed");
                return false;
            }
        }
    }
    out = p;
    return true;
}

// ---------- host suites ----------

struct Suites {
    SPBasicSuite* sp = nullptr;
    PrSDKGPUDeviceSuite* gpu = nullptr;
    PrSDKPPixSuite* ppix = nullptr;
    PrSDKVideoSegmentSuite* segment = nullptr;
    PrSDKMemoryManagerSuite* memory = nullptr;
};

// ---------- one instance = one effect on one clip with one set of parameters ----------

class Instance {
public:
    Suites s;
    csSDK_uint32 deviceIndex = 0;
    csSDK_int32 nodeID = 0;
    PrGPUDeviceInfo device{};
    Pipelines pipelines;
    lm::Engine engine;
    id<MTLBuffer> lutBuffer = nil;
    id<MTLBuffer> srcBuffer = nil;  // unchanged copy of the frame: the kernel grades in place, deband reads neighbours
    std::shared_ptr<const lm::Lut3D> match;

    ~Instance() {
        if (lutBuffer) [lutBuffer release];
        if (srcBuffer) [srcBuffer release];
    }

    // Effect Controls value of a parameter (AE index, input layer = 0) at clip time `t`.
    double param(int aeIndex, PrTime t, bool* ok = nullptr) const {
        PrParam p = {};
        prSuiteError e = s.segment->GetParam(nodeID, aeIndex - 1, t, &p);  // GPU filters don't count the input
        if (ok) *ok = PrSuiteErrorSucceeded(e);
        switch (p.mType) {
            case kPrParamType_Int8: return p.mInt8;
            case kPrParamType_Int16: return p.mInt16;
            case kPrParamType_Int32: return p.mInt32;
            case kPrParamType_Int64: return double(p.mInt64);
            case kPrParamType_Float32: return p.mFloat32;
            case kPrParamType_Float64: return p.mFloat64;
            case kPrParamType_Bool: return p.mBool ? 1 : 0;
            default: if (ok) *ok = false; return 0;
        }
    }

    // Same mapping as readParams() in LookMatch.cpp, except popups: the GPU filter API returns them
    // 0-based (Effect Controls and the AE API are 1-based).
    lm::Params params(PrTime t) const {
        lm::Params p;
        p.input = lm::InputSpace(std::max(0, int(param(P_INPUT_SPACE, t))));
        p.exposure = float(param(P_EXPOSURE, t));
        p.temperature = float(param(P_TEMPERATURE, t)) / 100.0f;
        p.tint = float(param(P_TINT, t)) / 100.0f;
        p.matchStrength = float(param(P_MATCH_STRENGTH, t)) / 100.0f;
        p.filmStrength = float(param(P_FILM_STRENGTH, t)) / 100.0f;
        p.film.negative = std::max(0, int(param(P_NEGATIVE, t)));
        p.film.print = std::max(0, int(param(P_PRINT, t)));
        p.film.printerPoints[0] = float(param(P_PRINTER_R, t));
        p.film.printerPoints[1] = float(param(P_PRINTER_G, t));
        p.film.printerPoints[2] = float(param(P_PRINTER_B, t));
        p.film.saturation = float(param(P_FILM_SAT, t)) / 100.0f;
        p.fade = float(param(P_FADE, t)) / 100.0f;
        p.halation = float(param(P_HALATION, t)) / 100.0f;
        p.halationRadius = float(param(P_HALATION_RADIUS, t));
        p.halationThreshold = float(param(P_HALATION_THRESHOLD, t));
        p.bloom = float(param(P_BLOOM, t)) / 100.0f;
        p.bloomRadius = float(param(P_BLOOM_RADIUS, t));
        p.bloomThreshold = float(param(P_BLOOM_THRESHOLD, t));
        p.grain = float(param(P_GRAIN, t)) / 100.0f;
        p.grainSize = float(param(P_GRAIN_SIZE, t));
        p.grainColor = float(param(P_GRAIN_COLOR, t)) / 100.0f;
        p.softness = float(param(P_SOFTNESS, t)) / 100.0f;
        p.vignette = float(param(P_VIGNETTE, t)) / 100.0f;
        p.weave = float(param(P_WEAVE, t));
        p.flicker = float(param(P_FLICKER, t)) / 100.0f;
        p.mix = float(param(P_MIX, t)) / 100.0f;
        return p;
    }

    // The match this clip uses, mirroring Render in LookMatch.cpp: a newer panel slot wins over
    // the stored Match Data. Returns false if it can't be known here (then the CPU renders).
    bool resolveMatch(PrTime t, std::string& why) {
        const long slot = long(param(P_REF_SLOT, t));
        PrParam mp = {};
        bool haveData = PrSuiteErrorSucceeded(s.segment->GetParam(nodeID, P_MATCH_DATA - 1, t, &mp)) &&
                        mp.mType == kPrParamType_PrMemoryPtr && mp.mMemoryPtr;
        const MatchDataV1* md = nullptr;
        if (haveData && lmpanel::matchDataUsable(mp.mMemoryPtr, s.memory->GetPtrSize(mp.mMemoryPtr)))
            md = reinterpret_cast<const MatchDataV1*>(mp.mMemoryPtr);
        const bool slotWins = slot > 0 && !(md && md->hasMatch && md->panelSlot == slot);
        gpuLog("node %d match: Match Data %s (type %d, %s), slot %ld, %s", int(nodeID), haveData ? "read" : "unreadable",
               int(mp.mType), md ? (md->hasMatch ? "has match" : "empty") : "invalid", slot, slotWins ? "slot wins" : "Match Data wins");
        if (slotWins) {
            match = lmpanel::readSlotLut(lmpanel::refsDir() + std::to_string(slot) + ".lut");
            if (!match) why = "panel match not built yet";
        } else if (md) {
            match = md->hasMatch ? lmpanel::lutFromMatch(md) : nullptr;
        } else if (!haveData || slot <= 0) {
            // Match Data unreadable here: only safe if the effect has never been matched, which we can't tell.
            why = "Match Data not readable by the GPU filter";
        }
        if (haveData) s.memory->PrDisposePtr(mp.mMemoryPtr);
        return why.empty();
    }

    // Panel requests (capture, export) are answered by the CPU render; leave those to it.
    bool panelRequestPending(PrTime t, std::string& why) const {
        struct { int param; const char* kind; } reqs[] = {
            {P_CAPTURE_SLOT, "capture"}, {P_EXPORT_SLOT, "export"}};
        for (auto& r : reqs) {
            long slot = long(param(r.param, t));
            if (slot > 0 && !lmpanel::fileExists(lmpanel::refsDir() + r.kind + "-" + std::to_string(slot) + ".done")) {
                why = std::string(r.kind) + " request pending";
                return true;
            }
        }
        return false;
    }
};

Suites gSuites;

prSuiteError CreateInstance(PrGPUFilterInstance* io) {
    if (gpuDisabled()) return suiteError_Fail;
    auto inst = std::make_unique<Instance>();
    inst->s = gSuites;
    inst->deviceIndex = io->inDeviceIndex;
    inst->nodeID = io->inNodeID;
    if (!inst->s.gpu || !inst->s.ppix || !inst->s.segment || !inst->s.memory) return suiteError_Fail;
    inst->s.gpu->GetDeviceInfo(kPrSDKGPUDeviceSuiteVersion, inst->deviceIndex, &inst->device);
    if (inst->device.outDeviceFramework != PrGPUDeviceFramework_Metal) return suiteError_Fail;
    if (!pipelinesFor(inst->deviceIndex, (id<MTLDevice>)inst->device.outDeviceHandle, inst->pipelines))
        return suiteError_Fail;

    // An instance is for one set of parameters: decide now whether the GPU can render it exactly.
    const PrTime t0 = 0;
    std::string why;
    lm::Params p = inst->params(t0);
    lm::FinishParams fp;
    inst->engine.setParams(p);
    if (p.input != lm::InputSpace::Rec709) why = "log footage";
    else if (p.halation > 0 || p.bloom > 0 || p.softness > 0 || p.weave > 0 || p.flicker > 0)
        why = "halation/bloom/softness/weave/flicker";
    else if (!inst->panelRequestPending(t0, why))
        inst->resolveMatch(t0, why);
    if (!why.empty()) {
        gpuLog("node %d renders on the CPU: %s", int(inst->nodeID), why.c_str());
        return suiteError_Fail;
    }
    inst->engine.setMatchLut(inst->match);
    {
        PrParam raw = {};
        inst->s.segment->GetParam(inst->nodeID, P_NEGATIVE - 1, t0, &raw);
        gpuLog("node %d params: encoding %.0f negative %.0f (type %d) print %.0f film %.1f sat %.1f grain %.1f "
               "vignette %.1f mix %.1f slot %.0f match %s", int(inst->nodeID), inst->param(P_INPUT_SPACE, t0),
               inst->param(P_NEGATIVE, t0), int(raw.mType), inst->param(P_PRINT, t0), inst->param(P_FILM_STRENGTH, t0),
               inst->param(P_FILM_SAT, t0), inst->param(P_GRAIN, t0), inst->param(P_VIGNETTE, t0), inst->param(P_MIX, t0),
               inst->param(P_REF_SLOT, t0), inst->match ? "yes" : "none");
    }
    io->ioPrivatePluginData = inst.release();
    io->outIsRealtime = 1;
    gpuLog("node %d renders on the GPU (device %u)", int(io->inNodeID), unsigned(io->inDeviceIndex));
    return suiteError_NoError;
}

prSuiteError DisposeInstance(PrGPUFilterInstance* io) {
    delete static_cast<Instance*>(io->ioPrivatePluginData);
    io->ioPrivatePluginData = nullptr;
    return suiteError_NoError;
}

prSuiteError GetFrameDependencies(PrGPUFilterInstance*, const PrGPUFilterRenderParams*, csSDK_int32*,
                                  PrGPUFilterFrameDependency*) {
    return suiteError_NotImplemented;  // only the current frame
}

prSuiteError Precompute(PrGPUFilterInstance*, const PrGPUFilterRenderParams*, csSDK_int32, PPixHand) {
    return suiteError_NotImplemented;
}

prSuiteError Render(PrGPUFilterInstance* io, const PrGPUFilterRenderParams* rp, const PPixHand* inFrames,
                    csSDK_size_t inFrameCount, PPixHand* outFrame) {
    Instance* inst = static_cast<Instance*>(io->ioPrivatePluginData);
    if (!inst || !outFrame) return suiteError_Fail;
    @autoreleasepool {
        // values may be keyframed: read them at this frame
        lm::Params p = inst->params(rp->inClipTime);
        inst->engine.setParams(p);
        prRect bounds = {};
        inst->s.ppix->GetBounds(*outFrame, &bounds);
        const int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
        const long frame = rp->inRenderTicksPerFrame ? long(rp->inClipTime / rp->inRenderTicksPerFrame) : 0;
        lm::FinishParams fp;
        if (!inst->engine.prepareDirect(w, h, frame, fp)) {
            gpuLog("node %d: settings changed to CPU-only mid-clip", int(inst->nodeID));
            return suiteError_Fail;
        }
        const lm::Lut3D& lut = inst->engine.directLut();
        const size_t entriesBytes = size_t(lut.size()) * lut.size() * lut.size() * sizeof(lm::Vec3);
        const size_t lutBytes = entriesBytes + lut.shaper().size() * sizeof(float);
        id<MTLDevice> device = (id<MTLDevice>)inst->device.outDeviceHandle;
        // The engine rebuilds its LUT in place when settings change; uploading it each frame (~3 MB,
        // shared memory) is cheap next to the frame itself and never shows a stale look.
        if (!inst->lutBuffer || inst->lutBuffer.length != lutBytes) {
            if (inst->lutBuffer) [inst->lutBuffer release];
            inst->lutBuffer = [device newBufferWithLength:lutBytes options:MTLResourceStorageModeShared];
        }
        std::memcpy(inst->lutBuffer.contents, lut.data(), entriesBytes);
        if (!lut.shaper().empty())
            std::memcpy(static_cast<char*>(inst->lutBuffer.contents) + entriesBytes, lut.shaper().data(), lut.shaper().size() * sizeof(float));

        void* data = nullptr;
        inst->s.gpu->GetGPUPPixData(*outFrame, &data);
        PrPixelFormat fmt = PrPixelFormat_Invalid;
        inst->s.ppix->GetPixelFormat(*outFrame, &fmt);
        csSDK_int32 rowBytes = 0;
        inst->s.ppix->GetRowBytes(*outFrame, &rowBytes);
        const bool f32 = fmt == PrPixelFormat_GPU_BGRA_4444_32f;
        const int pitch = rowBytes / (f32 ? 16 : 8);

        // split view (Compare group)
        const bool split = inst->param(P_SPLIT, rp->inClipTime) != 0;
        LookMatchGPUParams gp = makeGPUParams(fp, pitch, w, h, lut.size(), split,
                                              float(inst->param(P_SPLIT_POS, rp->inClipTime)) / 100.0f,
                                              float(inst->param(P_SPLIT_ANGLE, rp->inClipTime)));
        gp.shaperSize = int(lut.shaper().size());

        id<MTLCommandQueue> queue = (id<MTLCommandQueue>)inst->device.outCommandQueueHandle;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        const NSUInteger frameBytes = NSUInteger(rowBytes) * NSUInteger(h);
        if (!inst->srcBuffer || inst->srcBuffer.length < frameBytes) {
            if (inst->srcBuffer) [inst->srcBuffer release];
            inst->srcBuffer = [device newBufferWithLength:frameBytes options:MTLResourceStorageModePrivate];
        }
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:(id<MTLBuffer>)data sourceOffset:0 toBuffer:inst->srcBuffer destinationOffset:0 size:frameBytes];
        [blit endEncoding];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        id<MTLComputePipelineState> pso = f32 ? inst->pipelines.f32 : inst->pipelines.f16;
        [enc setComputePipelineState:pso];
        [enc setBuffer:(id<MTLBuffer>)data offset:0 atIndex:0];
        [enc setBytes:&gp length:sizeof gp atIndex:1];
        [enc setBuffer:inst->lutBuffer offset:0 atIndex:2];
        [enc setBuffer:inst->srcBuffer offset:0 atIndex:3];
        MTLSize group = MTLSizeMake(pso.threadExecutionWidth, 16, 1);
        MTLSize grid = MTLSizeMake((w + group.width - 1) / group.width, (h + group.height - 1) / group.height, 1);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:group];
        [enc endEncoding];
        [cb commit];
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3)
            gpuLog("node %d frame %ld %dx%d %s pitch %d", int(inst->nodeID), frame, w, h, f32 ? "32f" : "16f", pitch);
    }
    return suiteError_NoError;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) prSuiteError xGPUFilterEntry(
    csSDK_uint32 inHostInterfaceVersion, csSDK_int32* ioIndex, prBool inStartup, piSuitesPtr piSuites,
    PrGPUFilter* outFilter, PrGPUFilterInfo* outFilterInfo) {
    if (inStartup) {
        SPBasicSuite* sp = piSuites->utilFuncs->getSPBasicSuite();
        gSuites.sp = sp;
        sp->AcquireSuite(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion, (const void**)&gSuites.gpu);
        sp->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, (const void**)&gSuites.ppix);
        sp->AcquireSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion, (const void**)&gSuites.segment);
        sp->AcquireSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion, (const void**)&gSuites.memory);
        outFilter->CreateInstance = CreateInstance;
        outFilter->DisposeInstance = DisposeInstance;
        outFilter->GetFrameDependencies = GetFrameDependencies;
        outFilter->Precompute = Precompute;
        outFilter->Render = Render;
        outFilterInfo->outInterfaceVersion = PrSDKGPUFilterInterfaceVersion;
        outFilterInfo->outMatchName = PrSDKString();  // default: the module's PiPL match name
        gpuLog("GPU filter registered (host interface %u)", unsigned(inHostInterfaceVersion));
    } else if (gSuites.sp) {
        gSuites.sp->ReleaseSuite(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion);
        gSuites.sp->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        gSuites.sp->ReleaseSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion);
        gSuites.sp->ReleaseSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion);
        gSuites = Suites();
    }
    return suiteError_NoError;
}
