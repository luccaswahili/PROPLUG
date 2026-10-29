// LookMatch: Premiere Pro / After Effects effect wrapper around the portable engine in src/core.
//
// Pixel formats: Premiere BGRA 8-bit and BGRA 32-bit float, After Effects 8 and 16-bit.
// The heavy lifting (LUT bake, spatial passes, grain) lives in lm::Engine; this file only maps
// host parameters and pixel buffers onto it.
#include "LookMatch.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "matcher.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "stb_image_write.h"

namespace {

// ---------- engine pool ----------
// Hosts render frames on several threads at once. Each engine caches its baked colour LUT,
// so we keep a few around and hand them out instead of rebuilding a LUT per frame.
struct PoolEntry {
    lm::Engine engine;
    std::shared_ptr<const lm::Lut3D> lut;  // the match LUT the engine currently uses (or null)
    uint64_t matchId = ~0ull;
    bool busy = false;
};
std::mutex gPoolMutex;
std::vector<std::unique_ptr<PoolEntry>> gPool;

PoolEntry* acquireEngine() {
    std::lock_guard<std::mutex> lock(gPoolMutex);
    for (auto& e : gPool)
        if (!e->busy) { e->busy = true; return e.get(); }
    gPool.push_back(std::make_unique<PoolEntry>());
    gPool.back()->busy = true;
    return gPool.back().get();
}
void releaseEngine(PoolEntry* e) {
    std::lock_guard<std::mutex> lock(gPoolMutex);
    e->busy = false;
}

// ---------- parameters ----------

const char* kInputChoices = "Rec.709 / display|ARRI LogC3|Sony S-Log3|Panasonic V-Log";

std::string stockChoices(bool negatives) {
    std::string s;
    int n = negatives ? lm::negativeStockCount() : lm::printStockCount();
    for (int i = 0; i < n; ++i) {
        if (i) s += "|";
        s += negatives ? lm::negativeStock(i).name : lm::printStock(i).name;
    }
    return s;
}

float F(PF_ParamDef* params[], int i) { return float(params[i]->u.fs_d.value); }

lm::Params readParams(PF_ParamDef* params[]) {
    lm::Params p;
    p.input = lm::InputSpace(params[P_INPUT_SPACE]->u.pd.value - 1);
    p.exposure = F(params, P_EXPOSURE);
    p.temperature = F(params, P_TEMPERATURE) / 100.0f;
    p.tint = F(params, P_TINT) / 100.0f;
    p.matchStrength = F(params, P_MATCH_STRENGTH) / 100.0f;
    p.filmStrength = F(params, P_FILM_STRENGTH) / 100.0f;
    p.film.negative = params[P_NEGATIVE]->u.pd.value - 1;
    p.film.print = params[P_PRINT]->u.pd.value - 1;
    p.film.printerPoints[0] = F(params, P_PRINTER_R);
    p.film.printerPoints[1] = F(params, P_PRINTER_G);
    p.film.printerPoints[2] = F(params, P_PRINTER_B);
    p.film.saturation = F(params, P_FILM_SAT) / 100.0f;
    p.fade = F(params, P_FADE) / 100.0f;
    p.halation = F(params, P_HALATION) / 100.0f;
    p.halationRadius = F(params, P_HALATION_RADIUS);
    p.halationThreshold = F(params, P_HALATION_THRESHOLD);
    p.bloom = F(params, P_BLOOM) / 100.0f;
    p.bloomRadius = F(params, P_BLOOM_RADIUS);
    p.bloomThreshold = F(params, P_BLOOM_THRESHOLD);
    p.grain = F(params, P_GRAIN) / 100.0f;
    p.grainSize = F(params, P_GRAIN_SIZE);
    p.grainColor = F(params, P_GRAIN_COLOR) / 100.0f;
    p.softness = F(params, P_SOFTNESS) / 100.0f;
    p.vignette = F(params, P_VIGNETTE) / 100.0f;
    p.weave = F(params, P_WEAVE);
    p.flicker = F(params, P_FLICKER) / 100.0f;
    p.mix = F(params, P_MIX) / 100.0f;
    return p;
}

// Applies one control by its Effect Controls name and value (Effect Controls units: percentages,
// 1-based popups), mirroring readParams. Used for look previews requested by the panel.
bool applyNamedValue(lm::Params& p, const std::string& name, float v) {
    static const std::map<std::string, std::function<void(lm::Params&, float)>> setters = {
        {"Footage Encoding", [](lm::Params& q, float x) { q.input = lm::InputSpace(std::max(0, int(x) - 1)); }},
        {"Exposure", [](lm::Params& q, float x) { q.exposure = x; }},
        {"Temperature", [](lm::Params& q, float x) { q.temperature = x / 100.0f; }},
        {"Tint", [](lm::Params& q, float x) { q.tint = x / 100.0f; }},
        {"Match Strength", [](lm::Params& q, float x) { q.matchStrength = x / 100.0f; }},
        {"Film Strength", [](lm::Params& q, float x) { q.filmStrength = x / 100.0f; }},
        {"Negative Stock", [](lm::Params& q, float x) { q.film.negative = std::max(0, int(x) - 1); }},
        {"Print Stock", [](lm::Params& q, float x) { q.film.print = std::max(0, int(x) - 1); }},
        {"Printer Light R", [](lm::Params& q, float x) { q.film.printerPoints[0] = x; }},
        {"Printer Light G", [](lm::Params& q, float x) { q.film.printerPoints[1] = x; }},
        {"Printer Light B", [](lm::Params& q, float x) { q.film.printerPoints[2] = x; }},
        {"Film Saturation", [](lm::Params& q, float x) { q.film.saturation = x / 100.0f; }},
        {"Fade", [](lm::Params& q, float x) { q.fade = x / 100.0f; }},
        {"Halation", [](lm::Params& q, float x) { q.halation = x / 100.0f; }},
        {"Halation Size", [](lm::Params& q, float x) { q.halationRadius = x; }},
        {"Halation Threshold", [](lm::Params& q, float x) { q.halationThreshold = x; }},
        {"Bloom", [](lm::Params& q, float x) { q.bloom = x / 100.0f; }},
        {"Bloom Size", [](lm::Params& q, float x) { q.bloomRadius = x; }},
        {"Bloom Threshold", [](lm::Params& q, float x) { q.bloomThreshold = x; }},
        {"Grain", [](lm::Params& q, float x) { q.grain = x / 100.0f; }},
        {"Grain Size", [](lm::Params& q, float x) { q.grainSize = x; }},
        {"Grain Colour", [](lm::Params& q, float x) { q.grainColor = x / 100.0f; }},
        {"Softness", [](lm::Params& q, float x) { q.softness = x / 100.0f; }},
        {"Vignette", [](lm::Params& q, float x) { q.vignette = x / 100.0f; }},
        {"Gate Weave", [](lm::Params& q, float x) { q.weave = x; }},
        {"Flicker", [](lm::Params& q, float x) { q.flicker = x / 100.0f; }},
        {"Mix", [](lm::Params& q, float x) { q.mix = x / 100.0f; }},
    };
    auto it = setters.find(name);
    if (it == setters.end()) return false;
    it->second(p, v);
    return true;
}

void setError(PF_OutData* out_data, const char* msg) {
    // PF_STRCPY needs in_data in scope; a bounded copy does the same job here
    std::snprintf(out_data->return_msg, sizeof(out_data->return_msg), "%s", msg);
    out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
}

bool isPremiere(PF_InData* in_data) { return in_data->appl_id == 'PrMr'; }

// ---------- pixel conversion ----------

// ---------- diagnostics ----------
// Off by default. Set LOOKMATCH_LOG=1 in the host's environment (on macOS, for a host started
// from the Dock: `launchctl setenv LOOKMATCH_LOG 1`, then relaunch it) to append to
// ~/Library/Logs/LookMatch.log.

bool logEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("LOOKMATCH_LOG");
        return v && v[0] == '1';
    }();
    return on;
}

void lmLog(const char* fmt, ...) {
    if (!logEnabled()) return;
    static std::mutex m;
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/Library/Logs/LookMatch.log";
    std::lock_guard<std::mutex> lock(m);
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
    std::fprintf(f, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

std::string fourcc(uint32_t v) {
    char c[5] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char(v >> 24), 0};
    for (char& ch : c) if (ch && (ch < 32 || ch > 126)) ch = '?';
    return c;
}

// ---------- pixel formats ----------

enum class Fmt { AE8, AE16, PrBGRA8, PrBGRA16, PrBGRA32f, PrARGB8, PrARGB16, PrARGB32f, PrVUYA8, PrVUYA8_709, PrVUYA32f,
                 PrVUYA32f_709, Unsupported };

// Premiere reports its own pixel format for every world; After Effects only has ARGB 8/16 bit.
Fmt worldFormat(PF_InData* in_data, PF_EffectWorld* world, uint32_t* rawOut = nullptr) {
    if (rawOut) *rawOut = 0;
    if (isPremiere(in_data)) {
        AEFX_SuiteScoper<PF_PixelFormatSuite1> pfs(in_data, kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
        PrPixelFormat f = PrPixelFormat_Invalid;
        pfs->GetPixelFormat(world, &f);
        if (rawOut) *rawOut = uint32_t(f);
        switch (f) {
            case PrPixelFormat_BGRA_4444_8u: return Fmt::PrBGRA8;
            case PrPixelFormat_BGRA_4444_16u: return Fmt::PrBGRA16;
            case PrPixelFormat_BGRA_4444_32f: return Fmt::PrBGRA32f;
            case PrPixelFormat_ARGB_4444_8u: return Fmt::PrARGB8;
            case PrPixelFormat_ARGB_4444_16u: return Fmt::PrARGB16;
            case PrPixelFormat_ARGB_4444_32f: return Fmt::PrARGB32f;
            case PrPixelFormat_VUYA_4444_8u: return Fmt::PrVUYA8;
            case PrPixelFormat_VUYA_4444_8u_709: return Fmt::PrVUYA8_709;
            case PrPixelFormat_VUYA_4444_32f: return Fmt::PrVUYA32f;
            case PrPixelFormat_VUYA_4444_32f_709: return Fmt::PrVUYA32f_709;
            default: return Fmt::Unsupported;
        }
    }
    return PF_WORLD_IS_DEEP(world) ? Fmt::AE16 : Fmt::AE8;
}

// Formats Render can also write back (VUYA is only read, to sample a frame for matching).
bool canWrite(Fmt f) {
    return f == Fmt::AE8 || f == Fmt::AE16 || f == Fmt::PrBGRA8 || f == Fmt::PrBGRA32f;
}

// Y'CbCr (video range for 8 bit, Cb/Cr centred on 0.5 / 0) to R'G'B'
inline void yuvToRgb(float y, float cb, float cr, bool rec709, float* o) {
    const float kr = rec709 ? 0.2126f : 0.299f, kb = rec709 ? 0.0722f : 0.114f, kg = 1.0f - kr - kb;
    o[0] = y + 2.0f * (1.0f - kr) * cr;
    o[2] = y + 2.0f * (1.0f - kb) * cb;
    o[1] = (y - kr * o[0] - kb * o[2]) / kg;
}

inline char* rowPtr(PF_EffectWorld* w, int y) { return reinterpret_cast<char*>(w->data) + std::ptrdiff_t(y) * w->rowbytes; }

void worldToFloat(PF_EffectWorld* w, Fmt fmt, int width, int height, std::vector<float>& buf) {
    buf.resize(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        float* o = &buf[size_t(y) * width * 4];
        char* row = rowPtr(w, y);
        for (int x = 0; x < width; ++x, o += 4) {
            switch (fmt) {
                case Fmt::AE8: { PF_Pixel8* p = reinterpret_cast<PF_Pixel8*>(row) + x;
                    o[0] = p->red / 255.0f; o[1] = p->green / 255.0f; o[2] = p->blue / 255.0f; o[3] = p->alpha / 255.0f; break; }
                case Fmt::AE16: { PF_Pixel16* p = reinterpret_cast<PF_Pixel16*>(row) + x;
                    o[0] = p->red / 32768.0f; o[1] = p->green / 32768.0f; o[2] = p->blue / 32768.0f; o[3] = p->alpha / 32768.0f; break; }
                case Fmt::PrBGRA8: { const uint8_t* p = reinterpret_cast<uint8_t*>(row) + x * 4;
                    o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f; break; }
                case Fmt::PrBGRA32f: { const float* p = reinterpret_cast<float*>(row) + x * 4;
                    o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break; }
                case Fmt::PrBGRA16: { const uint16_t* p = reinterpret_cast<uint16_t*>(row) + x * 4;
                    o[0] = p[2] / 32768.0f; o[1] = p[1] / 32768.0f; o[2] = p[0] / 32768.0f; o[3] = p[3] / 32768.0f; break; }
                case Fmt::PrARGB8: { const uint8_t* p = reinterpret_cast<uint8_t*>(row) + x * 4;
                    o[0] = p[1] / 255.0f; o[1] = p[2] / 255.0f; o[2] = p[3] / 255.0f; o[3] = p[0] / 255.0f; break; }
                case Fmt::PrARGB16: { const uint16_t* p = reinterpret_cast<uint16_t*>(row) + x * 4;
                    o[0] = p[1] / 32768.0f; o[1] = p[2] / 32768.0f; o[2] = p[3] / 32768.0f; o[3] = p[0] / 32768.0f; break; }
                case Fmt::PrARGB32f: { const float* p = reinterpret_cast<float*>(row) + x * 4;
                    o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; o[3] = p[0]; break; }
                case Fmt::PrVUYA8: case Fmt::PrVUYA8_709: { const uint8_t* p = reinterpret_cast<uint8_t*>(row) + x * 4;
                    // byte order V, U, Y, A; video range 16-235 luma, 16-240 chroma
                    yuvToRgb((p[2] - 16) / 219.0f, (p[1] - 128) / 224.0f, (p[0] - 128) / 224.0f, fmt == Fmt::PrVUYA8_709, o);
                    o[3] = p[3] / 255.0f; break; }
                case Fmt::PrVUYA32f: case Fmt::PrVUYA32f_709: { const float* p = reinterpret_cast<float*>(row) + x * 4;
                    yuvToRgb(p[2], p[1], p[0], fmt == Fmt::PrVUYA32f_709, o);
                    o[3] = p[3]; break; }
                default: o[0] = o[1] = o[2] = o[3] = 0.0f; break;
            }
        }
    }
}

inline uint8_t to8(float v) { return uint8_t(lm::clamp01(v) * 255.0f + 0.5f); }
inline A_u_short to16(float v) { return A_u_short(lm::clamp01(v) * 32768.0f + 0.5f); }

void floatToWorld(const std::vector<float>& buf, int width, int height, PF_EffectWorld* w, Fmt fmt) {
    for (int y = 0; y < height; ++y) {
        const float* s = &buf[size_t(y) * width * 4];
        char* row = rowPtr(w, y);
        for (int x = 0; x < width; ++x, s += 4) {
            switch (fmt) {
                case Fmt::AE8: { PF_Pixel8* p = reinterpret_cast<PF_Pixel8*>(row) + x;
                    p->red = to8(s[0]); p->green = to8(s[1]); p->blue = to8(s[2]); p->alpha = to8(s[3]); break; }
                case Fmt::AE16: { PF_Pixel16* p = reinterpret_cast<PF_Pixel16*>(row) + x;
                    p->red = to16(s[0]); p->green = to16(s[1]); p->blue = to16(s[2]); p->alpha = to16(s[3]); break; }
                case Fmt::PrBGRA8: { uint8_t* p = reinterpret_cast<uint8_t*>(row) + x * 4;
                    p[2] = to8(s[0]); p[1] = to8(s[1]); p[0] = to8(s[2]); p[3] = to8(s[3]); break; }
                case Fmt::PrBGRA32f: { float* p = reinterpret_cast<float*>(row) + x * 4;
                    p[2] = s[0]; p[1] = s[1]; p[0] = s[2]; p[3] = s[3]; break; }
                default: break;
            }
        }
    }
}

// ---------- match data (hidden arbitrary parameter) ----------

void initMatchData(MatchData* md) {
    std::memset(md, 0, sizeof(MatchData));
    md->magic = kMatchMagic;
    md->version = kMatchVersion;
}

PF_Handle newMatchHandle(PF_InData* in_data) {
    PF_Handle h = PF_NEW_HANDLE(sizeof(MatchData));
    if (!h) return nullptr;
    initMatchData(reinterpret_cast<MatchData*>(PF_LOCK_HANDLE(h)));
    PF_UNLOCK_HANDLE(h);
    return h;
}

// Locks the handle and returns it if it holds a MatchData of this version (else null, unlocked).
MatchData* lockMatch(PF_InData* in_data, PF_Handle h) {
    if (!h) return nullptr;
    auto* md = reinterpret_cast<MatchData*>(PF_LOCK_HANDLE(h));
    if (md && PF_GET_HANDLE_SIZE(h) >= sizeof(MatchData) && md->magic == kMatchMagic && md->version == kMatchVersion)
        return md;
    PF_UNLOCK_HANDLE(h);
    return nullptr;
}

std::shared_ptr<lm::Lut3D> lutFromMatch(const MatchData* md) {
    auto lut = std::make_shared<lm::Lut3D>(kMatchLutSize);
    const uint16_t* v = md->lut;
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r, v += 3)
                lut->at(r, g, b) = {v[0] / 65535.0f, v[1] / 65535.0f, v[2] / 65535.0f};
    return lut;
}

PF_Err ArbitraryCallback(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    if (extra->id != D_MATCH_DATA) return PF_Err_NONE;
    switch (extra->which_function) {
        case PF_Arbitrary_NEW_FUNC:
            *extra->u.new_func_params.arbPH = newMatchHandle(in_data);
            return *extra->u.new_func_params.arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
        case PF_Arbitrary_DISPOSE_FUNC:
            if (extra->u.dispose_func_params.arbH) PF_DISPOSE_HANDLE(extra->u.dispose_func_params.arbH);
            return PF_Err_NONE;
        case PF_Arbitrary_COPY_FUNC: {
            PF_Handle dst = PF_NEW_HANDLE(sizeof(MatchData));
            if (!dst) return PF_Err_OUT_OF_MEMORY;
            auto* d = reinterpret_cast<MatchData*>(PF_LOCK_HANDLE(dst));
            if (MatchData* s = lockMatch(in_data, extra->u.copy_func_params.src_arbH)) {
                std::memcpy(d, s, sizeof(MatchData));
                PF_UNLOCK_HANDLE(extra->u.copy_func_params.src_arbH);
            } else {
                initMatchData(d);
            }
            PF_UNLOCK_HANDLE(dst);
            *extra->u.copy_func_params.dst_arbPH = dst;
            return PF_Err_NONE;
        }
        case PF_Arbitrary_FLAT_SIZE_FUNC:
            *extra->u.flat_size_func_params.flat_data_sizePLu = sizeof(MatchData);
            return PF_Err_NONE;
        case PF_Arbitrary_FLATTEN_FUNC: {
            auto& f = extra->u.flatten_func_params;
            if (f.buf_sizeLu < sizeof(MatchData)) return PF_Err_INTERNAL_STRUCT_DAMAGED;
            if (MatchData* md = lockMatch(in_data, f.arbH)) {
                std::memcpy(f.flat_dataPV, md, sizeof(MatchData));
                PF_UNLOCK_HANDLE(f.arbH);
            } else {
                initMatchData(reinterpret_cast<MatchData*>(f.flat_dataPV));
            }
            return PF_Err_NONE;
        }
        case PF_Arbitrary_UNFLATTEN_FUNC: {
            auto& u = extra->u.unflatten_func_params;
            PF_Handle h = newMatchHandle(in_data);
            if (!h) return PF_Err_OUT_OF_MEMORY;
            auto* src = reinterpret_cast<const MatchData*>(u.flat_dataPV);
            if (u.buf_sizeLu >= sizeof(MatchData) && src->magic == kMatchMagic && src->version == kMatchVersion) {
                std::memcpy(PF_LOCK_HANDLE(h), src, sizeof(MatchData));
                PF_UNLOCK_HANDLE(h);
            }
            *u.arbPH = h;
            return PF_Err_NONE;
        }
        case PF_Arbitrary_INTERP_FUNC: {
            // not keyframeable, but answer anyway: hold the left value
            PF_ArbParamsExtra copy = *extra;
            copy.which_function = PF_Arbitrary_COPY_FUNC;
            copy.u.copy_func_params.src_arbH = extra->u.interp_func_params.left_arbH;
            copy.u.copy_func_params.dst_arbPH = extra->u.interp_func_params.interpPH;
            return ArbitraryCallback(in_data, &copy);
        }
        case PF_Arbitrary_COMPARE_FUNC: {
            auto& c = extra->u.compare_func_params;
            uint64_t a = 0, b = 0;
            if (MatchData* md = lockMatch(in_data, c.a_arbH)) { a = md->hasMatch ? md->matchId : 0; PF_UNLOCK_HANDLE(c.a_arbH); }
            if (MatchData* md = lockMatch(in_data, c.b_arbH)) { b = md->hasMatch ? md->matchId : 0; PF_UNLOCK_HANDLE(c.b_arbH); }
            *c.compareP = a == b ? PF_ArbCompare_EQUAL : PF_ArbCompare_NOT_EQUAL;
            return PF_Err_NONE;
        }
        case PF_Arbitrary_PRINT_SIZE_FUNC:
            *extra->u.print_size_func_params.print_sizePLu = 1;
            return PF_Err_NONE;
        case PF_Arbitrary_PRINT_FUNC:
            if (extra->u.print_func_params.print_sizeLu > 0) extra->u.print_func_params.print_bufferPC[0] = 0;
            return PF_Err_NONE;
        case PF_Arbitrary_SCAN_FUNC:
            return PF_Err_CANNOT_PARSE_KEYFRAME_TEXT;
        default:
            return PF_Err_NONE;
    }
}

// ---------- commands ----------

PF_Err About(PF_InData* in_data, PF_OutData* out_data) {
    PF_SPRINTF(out_data->return_msg, "%s v%d.%d\r%s", LM_NAME, LM_MAJOR, LM_MINOR, LM_DESCRIPTION);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data) {
    out_data->my_version = PF_VERSION(LM_MAJOR, LM_MINOR, LM_BUG, PF_Stage_DEVELOP, LM_BUILD);
    // keep in sync with AE_Effect_Global_OutFlags in LookMatchPiPL.r
    out_data->out_flags = PF_OutFlag_NON_PARAM_VARY | PF_OutFlag_DEEP_COLOR_AWARE;
    out_data->out_flags2 = 0;
    if (isPremiere(in_data)) {
        AEFX_SuiteScoper<PF_PixelFormatSuite1> pfs(in_data, kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, out_data);
        pfs->ClearSupportedPixelFormats(in_data->effect_ref);
        pfs->AddSupportedPixelFormat(in_data->effect_ref, PrPixelFormat_BGRA_4444_32f);
        pfs->AddSupportedPixelFormat(in_data->effect_ref, PrPixelFormat_BGRA_4444_8u);
    }
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    static std::string negs = stockChoices(true), prints = stockChoices(false);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Look Match", D_MATCH_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON("Reference", "Load Reference...", 0, PF_ParamFlag_SUPERVISE, D_LOAD_REF);
    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON("Footage frame", "Match to This Frame", 0, PF_ParamFlag_SUPERVISE, D_REMATCH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Match Strength", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_MATCH_STRENGTH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Region Matching", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_REGION_STRENGTH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Auto Skin Protect", TRUE, 0, D_SKIN_AUTO);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Skin Protect", 0, 100, 0, 100, 30, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_SKIN_PROTECT);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(D_MATCH_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Input", D_INPUT_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Footage Encoding", int(lm::InputSpace::Count), 1, kInputChoices, D_INPUT_SPACE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Exposure", -4, 4, -2, 2, 0, PF_Precision_HUNDREDTHS, 0, 0, D_EXPOSURE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Temperature", -100, 100, -100, 100, 0, PF_Precision_TENTHS, 0, 0, D_TEMPERATURE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Tint", -100, 100, -100, 100, 0, PF_Precision_TENTHS, 0, 0, D_TINT);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(D_INPUT_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Film Print", D_FILM_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Film Strength", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_FILM_STRENGTH);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Negative Stock", lm::negativeStockCount(), 2, negs.c_str(), D_NEGATIVE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Print Stock", lm::printStockCount(), 2, prints.c_str(), D_PRINT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Printer Light R", -24, 24, -12, 12, 0, PF_Precision_TENTHS, 0, 0, D_PRINTER_R);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Printer Light G", -24, 24, -12, 12, 0, PF_Precision_TENTHS, 0, 0, D_PRINTER_G);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Printer Light B", -24, 24, -12, 12, 0, PF_Precision_TENTHS, 0, 0, D_PRINTER_B);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Film Saturation", 0, 200, 0, 200, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_FILM_SAT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fade", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_FADE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(D_FILM_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Film Texture", D_TEXTURE_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Halation", 0, 200, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_HALATION);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Halation Size", 0.1, 5, 0.25, 3, 1, PF_Precision_HUNDREDTHS, 0, 0, D_HALATION_RADIUS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Halation Threshold", 0, 4, 0.1, 2, 0.6, PF_Precision_HUNDREDTHS, 0, 0, D_HALATION_THRESHOLD);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Bloom", 0, 200, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_BLOOM);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Bloom Size", 0.1, 5, 0.25, 3, 1, PF_Precision_HUNDREDTHS, 0, 0, D_BLOOM_RADIUS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Bloom Threshold", 0, 4, 0.1, 2, 0.5, PF_Precision_HUNDREDTHS, 0, 0, D_BLOOM_THRESHOLD);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Grain", 0, 200, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_GRAIN);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Grain Size", 0.5, 6, 0.5, 4, 1, PF_Precision_HUNDREDTHS, 0, 0, D_GRAIN_SIZE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Grain Colour", 0, 100, 0, 100, 30, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_GRAIN_COLOR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Softness", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_SOFTNESS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Vignette", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_VIGNETTE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gate Weave", 0, 10, 0, 4, 0, PF_Precision_HUNDREDTHS, 0, 0, D_WEAVE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Flicker", 0, 100, 0, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_FLICKER);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(D_TEXTURE_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Mix", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, D_MIX);
    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON("Lumetri LUT", "Export .cube...", 0, PF_ParamFlag_SUPERVISE, D_EXPORT_LUT);
    AEFX_CLR_STRUCT(def);
    PF_Handle matchDefault = newMatchHandle(in_data);
    if (!matchDefault) return PF_Err_OUT_OF_MEMORY;
    PF_ADD_ARBITRARY2("Match Data", 1, 1, PF_ParamFlag_CANNOT_TIME_VARY, PF_PUI_NO_ECW_UI, matchDefault, D_MATCH_DATA,
                      nullptr);
    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_START_COLLAPSED;
    PF_ADD_TOPIC("Panel Link", D_PANEL_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Reference Slot", 0, 999999, 0, 999999, 0, PF_Precision_INTEGER, 0,
                         PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY, D_REF_SLOT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON("Panel match", "Save into Project", 0, PF_ParamFlag_SUPERVISE, D_BAKE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("LUT Export Slot", 0, 999999, 0, 999999, 0, PF_Precision_INTEGER, 0,
                         PF_ParamFlag_CANNOT_TIME_VARY, D_EXPORT_SLOT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Capture Slot", 0, 999999, 0, 999999, 0, PF_Precision_INTEGER, 0,
                         PF_ParamFlag_CANNOT_TIME_VARY, D_CAPTURE_SLOT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Thumbs Slot", 0, 999999, 0, 999999, 0, PF_Precision_INTEGER, 0,
                         PF_ParamFlag_CANNOT_TIME_VARY, D_THUMBS_SLOT);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(D_PANEL_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Compare", D_COMPARE_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Split View", FALSE, 0, D_SPLIT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Split Position", 0, 100, 0, 100, 50, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0,
                         D_SPLIT_POS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Split Angle", -90, 90, -90, 90, 0, PF_Precision_TENTHS, 0, 0, D_SPLIT_ANGLE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(D_COMPARE_TOPIC_END);

    out_data->num_params = P_NUM_PARAMS;
    return err;
}

// Builds before 1.0.1 kept the match in sequence data; it is no longer used. Free it if a project has it.
PF_Err SequenceSetdown(PF_InData* in_data, PF_OutData* out_data) {
    if (in_data->sequence_data) PF_DISPOSE_HANDLE(in_data->sequence_data);
    out_data->sequence_data = nullptr;
    return PF_Err_NONE;
}

// Premiere shows a "Media pending" card (yellow gradient with text) while footage is still loading,
// e.g. for the first renders after a project opens. Matching that card gives a wild look, so
// recognise it: nearly every pixel is a yellow with little blue.
bool isMediaPending(const std::vector<float>& buf, int width, int height) {
    size_t n = size_t(width) * height, step = std::max<size_t>(1, n / 20000), yellow = 0, total = 0;
    for (size_t i = 0; i < n; i += step, ++total) {
        float r = buf[i * 4], g = buf[i * 4 + 1], b = buf[i * 4 + 2];
        if (r > 0.25f && g > 0.25f && std::abs(r - g) < 0.12f && b < 0.6f * std::min(r, g)) ++yellow;
    }
    return total && yellow > total * 3 / 4;
}

// Builds a match LUT from a reference still and a frame (RGBA float, as read by worldToFloat).
// Returns false with a user-facing message if the reference can't be read or the frame is unusable.
bool buildMatch(const char* refPath, const std::vector<float>& buf, int width, int height, PF_ParamDef* params[],
                lm::Lut3D& lutOut, std::string& error) {
    int rw, rh, rn;
    unsigned char* ref8 = stbi_load(refPath, &rw, &rh, &rn, 3);
    if (!ref8) {
        error = "LookMatch: could not read the reference image (PNG, JPEG, TGA or BMP).";
        return false;
    }
    if (isMediaPending(buf, width, height)) {
        stbi_image_free(ref8);
        error = "LookMatch: Premiere is still loading this clip (Media pending). Try again in a moment.";
        lmLog("buildMatch: frame is Premiere's Media pending card, not matching");
        return false;
    }
    std::vector<lm::Vec3> refPx;
    size_t total = size_t(rw) * rh, step = std::max<size_t>(1, total / 250000);
    for (size_t i = 0; i < total; i += step) refPx.push_back({ref8[i * 3] / 255.0f, ref8[i * 3 + 1] / 255.0f, ref8[i * 3 + 2] / 255.0f});
    stbi_image_free(ref8);
    lmLog("buildMatch: reference %s (%dx%d, %d channels)", refPath, rw, rh, rn);

    std::vector<lm::Vec3> srcPx;
    lm::Engine e;
    e.setParams(readParams(params));
    size_t n = size_t(width) * height, st = std::max<size_t>(1, n / 250000);
    for (size_t i = 0; i < n; i += st) srcPx.push_back(e.neutralDisplay({buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2]}));
    if (srcPx.empty()) {
        error = "LookMatch: no footage under the playhead to match.";
        return false;
    }
    // A black, blank or unreadable frame would make the matcher stretch noise into a wild LUT
    // (posterised, oversaturated colour), so refuse to build a match from it.
    double sum[3] = {0, 0, 0}, sq = 0;
    for (const lm::Vec3& v : srcPx) {
        sum[0] += v.r; sum[1] += v.g; sum[2] += v.b;
        double l = 0.2126 * v.r + 0.7152 * v.g + 0.0722 * v.b;
        sq += l * l;
    }
    const double cnt = double(srcPx.size());
    const double meanL = (0.2126 * sum[0] + 0.7152 * sum[1] + 0.0722 * sum[2]) / cnt;
    const double sdL = std::sqrt(std::max(0.0, sq / cnt - meanL * meanL));
    lmLog("buildMatch: source %dx%d mean RGB %.3f %.3f %.3f, luma sd %.4f", width, height, sum[0] / cnt, sum[1] / cnt,
          sum[2] / cnt, sdL);
    if (meanL < 0.01 || sdL < 0.01) {
        error = "LookMatch: the frame under the playhead is black or blank, so there is nothing to match. "
                "Move the playhead onto a normal frame and match again.";
        return false;
    }

    lm::MatchOptions mo;
    mo.regionStrength = F(params, P_REGION_STRENGTH) / 100.0f;
    mo.skinProtect = params[P_SKIN_AUTO]->u.bd.value ? -1.0f : F(params, P_SKIN_PROTECT) / 100.0f;
    mo.lutSize = kMatchLutSize;
    lutOut = lm::matchLook(refPx, srcPx, mo).lut;
    for (float g : {0.18f, 0.5f, 1.0f}) {
        lm::Vec3 v = lutOut.apply({g, g, g});
        lmLog("buildMatch: LUT grey %.2f -> %.3f %.3f %.3f", g, v.r, v.g, v.b);
    }
    return true;
}

PF_Err storeMatch(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], const lm::Lut3D& lut,
                  const char* refPath);

// Compute a match from the stored reference and the input layer at the current time.
PF_Err ComputeMatch(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], const char* refPath) {
    PF_ParamDef layer;
    AEFX_CLR_STRUCT(layer);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, P_INPUT_LAYER, in_data->current_time, in_data->time_step, in_data->time_scale, &layer);
    if (err) return err;
    PF_EffectWorld* world = &layer.u.ld;
    uint32_t rawFmt = 0;
    Fmt fmt = Fmt::Unsupported;
    if (world->data) fmt = worldFormat(in_data, world, &rawFmt);
    lmLog("ComputeMatch: checked-out layer data=%p %dx%d rowbytes=%d format=%s (%u)", world->data, world->width,
          world->height, int(world->rowbytes), fourcc(rawFmt).c_str(), unsigned(fmt));
    std::vector<float> buf;
    const int width = world->width, height = world->height;
    if (world->data && fmt != Fmt::Unsupported) worldToFloat(world, fmt, width, height, buf);
    PF_CHECKIN_PARAM(in_data, &layer);
    if (buf.empty()) {
        std::string msg = world->data ? "LookMatch: this clip's pixel format (" + fourcc(rawFmt) + ") isn't supported yet."
                                      : "LookMatch: no footage under the playhead to match.";
        setError(out_data, msg.c_str());
        return PF_Err_NONE;
    }
    lm::Lut3D lut(kMatchLutSize);
    std::string error;
    if (!buildMatch(refPath, buf, width, height, params, lut, error)) {
        setError(out_data, error.c_str());
        return PF_Err_NONE;
    }
    return storeMatch(in_data, out_data, params, lut, refPath);
}

// Stores a match in the hidden parameter and tells the host its value changed, so it reaches
// every render instance and is saved in the project.
PF_Err storeMatch(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], const lm::Lut3D& lut,
                  const char* refPath) {
    PF_ParamDef* mp = params[P_MATCH_DATA];
    if (!mp->u.arb_d.value) mp->u.arb_d.value = newMatchHandle(in_data);
    MatchData* md = lockMatch(in_data, mp->u.arb_d.value);
    if (!md) {
        // wrong size or version: replace it with a fresh one
        PF_DISPOSE_HANDLE(mp->u.arb_d.value);
        mp->u.arb_d.value = newMatchHandle(in_data);
        md = lockMatch(in_data, mp->u.arb_d.value);
        if (!md) return PF_Err_OUT_OF_MEMORY;
    }
    uint16_t* q = md->lut;
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r, q += 3) {
                const lm::Vec3& v = lut.at(r, g, b);
                q[0] = uint16_t(lm::clamp01(v.r) * 65535.0f + 0.5f);
                q[1] = uint16_t(lm::clamp01(v.g) * 65535.0f + 0.5f);
                q[2] = uint16_t(lm::clamp01(v.b) * 65535.0f + 0.5f);
            }
    std::memset(md->referencePath, 0, sizeof(md->referencePath));
    std::strncpy(md->referencePath, refPath, sizeof(md->referencePath) - 1);
    md->hasMatch = 1;
    md->panelSlot = int32_t(F(params, P_REF_SLOT));  // this match supersedes the panel's current slot
    md->matchId = std::random_device{}() * 0x100000001ull + std::random_device{}();
    lmLog("storeMatch: stored match %016llx in the Match Data parameter", (unsigned long long)md->matchId);
    PF_UNLOCK_HANDLE(mp->u.arb_d.value);
    mp->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    out_data->out_flags |= PF_OutFlag_FORCE_RERENDER;
    return PF_Err_NONE;
}

// The LookMatch panel can't pass a string to an effect, so it writes the reference path to
// <app support>/PRO PLUGS/LookMatch/refs/<slot>.txt and sets Reference Slot = <slot>.
// Debug aid (only with LOOKMATCH_LOG=1): save the frame a match was built from.
std::vector<unsigned char> to8bitRGB(const std::vector<float>& buf, int width, int height) {
    std::vector<unsigned char> px(size_t(width) * height * 3);
    for (size_t i = 0; i < size_t(width) * height; ++i)
        for (int c = 0; c < 3; ++c) px[i * 3 + c] = uint8_t(lm::clamp01(buf[i * 4 + c]) * 255.0f + 0.5f);
    return px;
}

void dumpFrame(const std::string& file, const std::vector<float>& buf, int width, int height) {
    stbi_write_png(file.c_str(), width, height, 3, to8bitRGB(buf, width, height).data(), width * 3);
}

// Box-filters an RGBA float frame down to at most `tw` pixels wide (tw/th return the size).
std::vector<float> downscale(const std::vector<float>& src, int w, int h, int& tw, int& th) {
    tw = std::min(tw, w);
    th = std::max(1, int(std::lround(double(h) * tw / w)));
    std::vector<float> out(size_t(tw) * th * 4, 0.0f);
    for (int y = 0; y < th; ++y) {
        int y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
        for (int x = 0; x < tw; ++x) {
            int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw);
            float acc[4] = {0, 0, 0, 0};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx)
                    for (int c = 0; c < 4; ++c) acc[c] += src[(size_t(yy) * w + xx) * 4 + c];
            float n = float((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c) out[(size_t(y) * tw + x) * 4 + c] = acc[c] / n;
        }
    }
    return out;
}

// Renders the look previews the panel asked for (see the Thumbs Slot block in Render).
void renderThumbnails(const std::string& base, const std::vector<float>& frame, int w, int h, const lm::Params& clip,
                      std::shared_ptr<const lm::Lut3D> matchLut) {
    std::string request = base + ".txt", result = "ok";
    FILE* f = std::fopen(request.c_str(), "rb");
    if (!f) result = "no request";
    int tw = 320, th = 0;
    std::vector<float> small = f ? downscale(frame, w, h, tw, th) : std::vector<float>();
    int count = 0;
    char line[4096];
    while (f && std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t tab = s.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        std::string id = s.substr(0, tab);
        if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) continue;
        lm::Params p = clip;
        p.lutSize = 33;  // previews: a smaller colour LUT is plenty and much faster to build
        size_t pos = tab + 1;
        while (pos < s.size()) {
            size_t end = s.find(';', pos);
            std::string kv = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) applyNamedValue(p, kv.substr(0, eq), std::strtof(kv.c_str() + eq + 1, nullptr));
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        // Grain and weave are per pixel: at full size, shrinking the frame averages grain away by about
        // the scale factor, so scale them to look like a downsized full-resolution render.
        const float scale = float(tw) / float(w);
        p.grain *= scale;
        p.weave *= scale;
        lm::Engine e;
        e.setParams(p);
        e.setMatchLut(matchLut);
        std::vector<float> img = small;
        lm::ImageView v{img.data(), tw, th, tw * 4};
        e.render(v, v, 0);
        if (stbi_write_png((base + "-" + id + ".png").c_str(), tw, th, 3, to8bitRGB(img, tw, th).data(), tw * 3)) ++count;
    }
    if (f) std::fclose(f);
    if (FILE* df = std::fopen((base + ".done").c_str(), "wb")) { std::fputs(result.c_str(), df); std::fclose(df); }
    lmLog("renderThumbnails: %s -> %d previews (%s)", base.c_str(), count, result.c_str());
}

// Before/after wipe: pixels left of (or above) the split line show the original, with a thin line.
void applySplit(std::vector<float>& after, const std::vector<float>& before, int w, int h, float pos, float angleDeg) {
    const float a = angleDeg * 3.14159265f / 180.0f;
    const float nx = std::cos(a), ny = std::sin(a);  // line normal; angle 0 = vertical line
    const float cx = pos * w, cy = 0.5f * h;
    const float line = std::max(1.0f, h / 540.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float d = (x + 0.5f - cx) * nx + (y + 0.5f - cy) * ny;
            size_t i = (size_t(y) * w + x) * 4;
            if (std::abs(d) < line) { after[i] = after[i + 1] = after[i + 2] = 0.9f; }
            else if (d < 0) { after[i] = before[i]; after[i + 1] = before[i + 1]; after[i + 2] = before[i + 2]; }
        }
}

std::string refsDir() {
#ifdef AE_OS_WIN
    const char* base = std::getenv("APPDATA");
    return base ? std::string(base) + "\\PRO PLUGS\\LookMatch\\refs\\" : "";
#else
    const char* base = std::getenv("HOME");
    return base ? std::string(base) + "/Library/Application Support/PRO PLUGS/LookMatch/refs/" : "";
#endif
}

// First line of a small text file the panel wrote (a path), or "" if missing.
std::string panelFileLine(const std::string& file) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return "";
    char buf[1024] = {0};
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    std::string path(buf, n);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    return path;
}

std::string panelReferencePath(long slot) {
    std::string dir = refsDir();
    return dir.empty() ? "" : panelFileLine(dir + std::to_string(slot) + ".txt");
}

// Premiere does not send PF_Cmd_USER_CHANGED_PARAM when a panel sets a parameter, so a new
// Reference Slot is noticed in Render: the first render builds the match from its own input
// frame and every other render (any thread, any instance) reuses it via this cache, which is
// also written to refs/<slot>.lut so other effect instances and later sessions get the same LUT.
constexpr uint32_t kSlotLutMagic = 0x4C4D534C;  // 'LMSL'

std::shared_ptr<const lm::Lut3D> readSlotLut(const std::string& file) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return nullptr;
    uint32_t hdr[2] = {0, 0};
    std::vector<float> v(size_t(kMatchLutSize) * kMatchLutSize * kMatchLutSize * 3);
    bool ok = std::fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == kSlotLutMagic && hdr[1] == uint32_t(kMatchLutSize) &&
              std::fread(v.data(), sizeof(float), v.size(), f) == v.size();
    std::fclose(f);
    if (!ok) return nullptr;
    auto lut = std::make_shared<lm::Lut3D>(kMatchLutSize);
    const float* q = v.data();
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r, q += 3) lut->at(r, g, b) = {q[0], q[1], q[2]};
    return lut;
}

void writeSlotLut(const std::string& file, const lm::Lut3D& lut) {
    std::string tmp = file + ".tmp" + std::to_string(std::random_device{}());
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    uint32_t hdr[2] = {kSlotLutMagic, uint32_t(kMatchLutSize)};
    std::fwrite(hdr, sizeof hdr, 1, f);
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r) {
                const lm::Vec3& c = lut.at(r, g, b);
                float q[3] = {c.r, c.g, c.b};
                std::fwrite(q, sizeof q, 1, f);
            }
    std::fclose(f);
    std::rename(tmp.c_str(), file.c_str());  // atomic: readers see the old file or the whole new one
}

// Reads the frame to match into buf (RGBA float); returns false if there is none.
using FrameSource = std::function<bool(std::vector<float>& buf, int& width, int& height)>;

std::shared_ptr<const lm::Lut3D> slotLut(long slot, const FrameSource& getFrame, PF_ParamDef* params[]) {
    static std::mutex m;
    static std::map<long, std::shared_ptr<const lm::Lut3D>> cache;  // null entry = failed, don't retry every frame
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(slot);
    if (it != cache.end()) return it->second;
    const std::string file = refsDir() + std::to_string(slot) + ".lut";
    std::shared_ptr<const lm::Lut3D> lut = readSlotLut(file);
    if (lut) {
        lmLog("Render: slot %ld loaded from %s", slot, file.c_str());
    } else {
        std::string ref = panelReferencePath(slot), error;
        auto built = std::make_shared<lm::Lut3D>(kMatchLutSize);
        std::vector<float> frame;
        int width = 0, height = 0;
        if (!ref.empty() && !getFrame(frame, width, height)) error = "could not read the frame";
        if (error.empty() && !ref.empty() && isMediaPending(frame, width, height)) {
            lmLog("Render: slot %ld waits: the frame is Premiere's Media pending card", slot);
            return nullptr;  // not cached: try again on the next render
        }
        else if (logEnabled() && !ref.empty()) dumpFrame(refsDir() + std::to_string(slot) + "-source.png", frame, width, height);
        if (!ref.empty() && error.empty() && buildMatch(ref.c_str(), frame, width, height, params, *built, error)) {
            writeSlotLut(file, *built);
            lut = built;
            lmLog("Render: slot %ld matched to %s", slot, ref.c_str());
        } else {
            lmLog("Render: slot %ld could not be matched: %s", slot, ref.empty() ? "no reference file" : error.c_str());
        }
    }
    cache[slot] = lut;
    return lut;
}

PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_UserChangedParamExtra* extra) {
    switch (extra->param_index) {
        case P_LOAD_REF: {
            char path[1024] = {0};
            if (!LM_OpenImageDialog(path, sizeof path)) return PF_Err_NONE;
            return ComputeMatch(in_data, out_data, params, path);
        }
        case P_REMATCH: {
            std::string path;
            PF_Handle mh = params[P_MATCH_DATA]->u.arb_d.value;
            if (MatchData* md = lockMatch(in_data, mh)) {
                path = md->referencePath;
                PF_UNLOCK_HANDLE(mh);
            }
            if (path.empty()) { setError(out_data, "LookMatch: load a reference image first."); return PF_Err_NONE; }
            return ComputeMatch(in_data, out_data, params, path.c_str());
        }
        case P_REF_SLOT: {
            long slot = long(F(params, P_REF_SLOT));
            lmLog("UserChangedParam: Reference Slot = %ld", slot);
            if (slot <= 0) return PF_Err_NONE;
            std::string path = panelReferencePath(slot);
            if (path.empty()) { setError(out_data, "LookMatch: the panel's reference for this clip is missing."); return PF_Err_NONE; }
            return ComputeMatch(in_data, out_data, params, path.c_str());
        }
        case P_BAKE: {
            long slot = long(F(params, P_REF_SLOT));
            if (slot <= 0) { setError(out_data, "LookMatch: this clip has no match from the panel to save."); return PF_Err_NONE; }
            std::shared_ptr<const lm::Lut3D> lut = readSlotLut(refsDir() + std::to_string(slot) + ".lut");
            if (!lut) {
                setError(out_data, "LookMatch: the panel's match isn't built yet. Play over the clip once, then click "
                                   "Save into Project again.");
                return PF_Err_NONE;
            }
            std::string ref = panelReferencePath(slot);
            lmLog("Bake: slot %ld -> Match Data", slot);
            return storeMatch(in_data, out_data, params, *lut, ref.c_str());
        }
        case P_EXPORT_LUT: {
            char path[1024] = {0};
            if (!LM_SaveCubeDialog(path, sizeof path)) return PF_Err_NONE;
            lm::Engine e;
            e.setParams(readParams(params));
            PF_Handle mh = params[P_MATCH_DATA]->u.arb_d.value;
            if (MatchData* md = lockMatch(in_data, mh)) {
                if (md->hasMatch) e.setMatchLut(lutFromMatch(md));
                PF_UNLOCK_HANDLE(mh);
            }
            if (!e.bakeExportLut(65).writeCube(path, "LookMatch"))
                setError(out_data, "LookMatch: could not write the LUT file.");
            return PF_Err_NONE;
        }
        default:
            return PF_Err_NONE;
    }
}

PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    PF_EffectWorld* input = &params[P_INPUT_LAYER]->u.ld;
    uint32_t rawIn = 0, rawOut = 0;
    Fmt fmt = worldFormat(in_data, output, &rawOut);
    Fmt inFmt = worldFormat(in_data, input, &rawIn);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 3)
        lmLog("Render: in %s %dx%d rowbytes=%d, out %s %dx%d rowbytes=%d", fourcc(rawIn).c_str(), input->width,
              input->height, int(input->rowbytes), fourcc(rawOut).c_str(), output->width, output->height,
              int(output->rowbytes));
    if (!canWrite(fmt) || inFmt != fmt) {
        setError(out_data, "LookMatch: unsupported pixel format.");
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    const int w = std::min(input->width, output->width), h = std::min(input->height, output->height);
    if (w <= 0 || h <= 0) return PF_Err_NONE;

    thread_local std::vector<float> buf;
    worldToFloat(input, fmt, w, h, buf);

    PoolEntry* pe = acquireEngine();
    pe->engine.setParams(readParams(params));
    // Which match to use: the one stored by Load Reference / Match to This Frame, unless the panel
    // has set a newer Reference Slot since then.
    const long slot = long(F(params, P_REF_SLOT));
    PF_Handle mh = params[P_MATCH_DATA]->u.arb_d.value;
    MatchData* md = lockMatch(in_data, mh);
    const bool slotWins = slot > 0 && !(md && md->hasMatch && md->panelSlot == slot);
    uint64_t id = !slotWins && md && md->hasMatch ? md->matchId : 0;
    std::shared_ptr<const lm::Lut3D> lut = id && id != pe->matchId ? lutFromMatch(md) : nullptr;
    if (md) PF_UNLOCK_HANDLE(mh);
    if (slotWins) {
        constexpr uint64_t kSlotIdBase = 0x534C4F5400000000ull;  // 'SLOT'
        // Match from the frame this render is drawing for this clip (Render's input, before grading).
        // Checking out the layer at current_time is wrong here: in Premiere it returns whatever is on
        // the track at that sequence time, which for a clip that isn't under the playhead is another clip.
        lut = slotLut(slot, [&](std::vector<float>& f, int& fw, int& fh) {
                          f = buf;
                          fw = w;
                          fh = h;
                          lmLog("Render: slot %ld source = this render's input (t=%d/%u, %dx%d)", slot,
                                int(in_data->current_time), unsigned(in_data->time_scale), w, h);
                          return true;
                      }, params);
        id = lut ? kSlotIdBase | uint64_t(slot) : 0;
    }
    if (id != pe->matchId) {
        pe->lut = id ? lut : nullptr;
        pe->engine.setMatchLut(pe->lut);
        pe->matchId = id;
        lmLog("Render: now using match %016llx", (unsigned long long)id);
    }
    // Panel "Export .cube": a new LUT Export Slot means write the current look to the path the panel
    // left in refs/export-<slot>.txt, once per slot, and report in refs/export-<slot>.done.
    const long exportSlot = long(F(params, P_EXPORT_SLOT));
    if (exportSlot > 0) {
        static std::mutex em;
        static std::set<long> done;
        std::lock_guard<std::mutex> lock(em);
        if (done.insert(exportSlot).second) {
            const std::string base = refsDir() + "export-" + std::to_string(exportSlot);
            std::string target = panelFileLine(base + ".txt"), result = "ok";
            if (target.empty()) result = "no export path";
            else if (!pe->engine.bakeExportLut(65).writeCube(target, "LookMatch")) result = "could not write " + target;
            if (FILE* f = std::fopen((base + ".done").c_str(), "wb")) { std::fputs(result.c_str(), f); std::fclose(f); }
            lmLog("Render: export slot %ld -> %s (%s)", exportSlot, target.c_str(), result.c_str());
        }
    }
    // Panel look previews: a new Thumbs Slot means render this frame (small, with this clip's match)
    // through each look listed in refs/thumbs-<slot>.txt ("id<TAB>Name=value;Name=value") and write
    // refs/thumbs-<slot>-<id>.png. Once per slot.
    const long thumbsSlot = long(F(params, P_THUMBS_SLOT));
    if (thumbsSlot > 0 && !isMediaPending(buf, w, h)) {
        static std::mutex tm;
        static std::set<long> rendered;
        std::lock_guard<std::mutex> lock(tm);
        if (rendered.insert(thumbsSlot).second)
            renderThumbnails(refsDir() + "thumbs-" + std::to_string(thumbsSlot), buf, w, h, readParams(params), pe->lut);
    }

    // Panel "hero clip": a new Capture Slot means save this clip's (ungraded) frame as a PNG the
    // panel then uses as the reference for other clips. Once per slot.
    const long captureSlot = long(F(params, P_CAPTURE_SLOT));
    if (captureSlot > 0) {
        static std::mutex cm;
        static std::set<long> captured;
        std::lock_guard<std::mutex> lock(cm);
        if (captured.insert(captureSlot).second) {
            const std::string base = refsDir() + "capture-" + std::to_string(captureSlot);
            std::string result = "ok";
            if (isMediaPending(buf, w, h)) {
                captured.erase(captureSlot);  // footage still loading: try again on the next render
                result.clear();
            } else if (!stbi_write_png((base + ".png").c_str(), w, h, 3, to8bitRGB(buf, w, h).data(), w * 3)) {
                result = "could not write " + base + ".png";
            }
            if (!result.empty()) {
                if (FILE* df = std::fopen((base + ".done").c_str(), "wb")) { std::fputs(result.c_str(), df); std::fclose(df); }
                lmLog("Render: capture slot %ld -> %s.png (%s)", captureSlot, base.c_str(), result.c_str());
            }
        }
    }
    long frame = in_data->time_step ? long(in_data->current_time / in_data->time_step) : 0;
    const bool split = params[P_SPLIT]->u.bd.value != 0;
    thread_local std::vector<float> before;
    if (split) before = buf;
    lm::ImageView view{buf.data(), w, h, w * 4};
    pe->engine.render(view, view, frame);
    if (split) applySplit(buf, before, w, h, F(params, P_SPLIT_POS) / 100.0f, F(params, P_SPLIT_ANGLE));
    releaseEngine(pe);

    floatToWorld(buf, w, h, output, fmt);
    return PF_Err_NONE;
}

}  // namespace

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion) {
    PF_Err result = PF_Err_INVALID_CALLBACK;  // PF_REGISTER_EFFECT_EXT2 assigns to a local named result
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, LM_NAME, "PROPLUGS LookMatch", "PRO PLUGS",
                                     AE_RESERVED_INFO, "EffectMain", "https://github.com/luccaswahili");
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra) {
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT: return About(in_data, out_data);
            case PF_Cmd_GLOBAL_SETUP: return GlobalSetup(in_data, out_data);
            case PF_Cmd_PARAMS_SETUP: return ParamsSetup(in_data, out_data);
            case PF_Cmd_SEQUENCE_SETDOWN: return SequenceSetdown(in_data, out_data);
            case PF_Cmd_ARBITRARY_CALLBACK: return ArbitraryCallback(in_data, reinterpret_cast<PF_ArbParamsExtra*>(extra));
            case PF_Cmd_USER_CHANGED_PARAM:
                return UserChangedParam(in_data, out_data, params, reinterpret_cast<PF_UserChangedParamExtra*>(extra));
            case PF_Cmd_RENDER: return Render(in_data, out_data, params, output);
            default: return PF_Err_NONE;
        }
    } catch (const std::bad_alloc&) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}
