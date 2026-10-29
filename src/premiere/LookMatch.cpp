// LookMatch: Premiere Pro / After Effects effect wrapper around the portable engine in src/core.
//
// Pixel formats: Premiere BGRA 8-bit and BGRA 32-bit float, After Effects 8 and 16-bit.
// The heavy lifting (LUT bake, spatial passes, grain) lives in lm::Engine; this file only maps
// host parameters and pixel buffers onto it.
#include "LookMatch.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
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

namespace {

// ---------- engine pool ----------
// Hosts render frames on several threads at once. Each engine caches its baked colour LUT,
// so we keep a few around and hand them out instead of rebuilding a LUT per frame.
struct PoolEntry {
    lm::Engine engine;
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

void setError(PF_OutData* out_data, const char* msg) {
    // PF_STRCPY needs in_data in scope; a bounded copy does the same job here
    std::snprintf(out_data->return_msg, sizeof(out_data->return_msg), "%s", msg);
    out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
}

bool isPremiere(PF_InData* in_data) { return in_data->appl_id == 'PrMr'; }

// ---------- pixel conversion ----------

enum class Fmt { AE8, AE16, PrBGRA8, PrBGRA32f, Unsupported };

Fmt worldFormat(PF_InData* in_data, PF_EffectWorld* world) {
    if (isPremiere(in_data)) {
        AEFX_SuiteScoper<PF_PixelFormatSuite1> pfs(in_data, kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
        PrPixelFormat f = PrPixelFormat_Invalid;
        pfs->GetPixelFormat(world, &f);
        if (f == PrPixelFormat_BGRA_4444_8u) return Fmt::PrBGRA8;
        if (f == PrPixelFormat_BGRA_4444_32f) return Fmt::PrBGRA32f;
        return Fmt::Unsupported;
    }
    return PF_WORLD_IS_DEEP(world) ? Fmt::AE16 : Fmt::AE8;
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
                default: break;
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

// ---------- sequence data ----------

SeqData* lockSeq(PF_InData* in_data) {
    if (!in_data->sequence_data) return nullptr;
    auto* sd = reinterpret_cast<SeqData*>(PF_LOCK_HANDLE(in_data->sequence_data));
    if (sd && (sd->magic != kSeqMagic || sd->version != kSeqVersion)) {
        PF_UNLOCK_HANDLE(in_data->sequence_data);
        return nullptr;
    }
    return sd;
}
void unlockSeq(PF_InData* in_data) { PF_UNLOCK_HANDLE(in_data->sequence_data); }

std::shared_ptr<lm::Lut3D> lutFromSeq(const SeqData* sd) {
    auto lut = std::make_shared<lm::Lut3D>(kMatchLutSize);
    const float* f = sd->matchLut;
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r, f += 3) lut->at(r, g, b) = {f[0], f[1], f[2]};
    return lut;
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

    out_data->num_params = P_NUM_PARAMS;
    return err;
}

PF_Err SequenceSetup(PF_InData* in_data, PF_OutData* out_data) {
    if (in_data->sequence_data) PF_DISPOSE_HANDLE(in_data->sequence_data);
    PF_Handle h = PF_NEW_HANDLE(sizeof(SeqData));
    if (!h) return PF_Err_OUT_OF_MEMORY;
    auto* sd = reinterpret_cast<SeqData*>(PF_LOCK_HANDLE(h));
    std::memset(sd, 0, sizeof(SeqData));
    sd->magic = kSeqMagic;
    sd->version = kSeqVersion;
    PF_UNLOCK_HANDLE(h);
    out_data->sequence_data = h;
    return PF_Err_NONE;
}

PF_Err SequenceResetup(PF_InData* in_data, PF_OutData* out_data) {
    if (!in_data->sequence_data) return SequenceSetup(in_data, out_data);
    SeqData* sd = lockSeq(in_data);
    if (!sd) return SequenceSetup(in_data, out_data);  // unknown version: start clean
    unlockSeq(in_data);
    return PF_Err_NONE;
}

PF_Err SequenceSetdown(PF_InData* in_data, PF_OutData* out_data) {
    if (in_data->sequence_data) PF_DISPOSE_HANDLE(in_data->sequence_data);
    out_data->sequence_data = nullptr;
    return PF_Err_NONE;
}

// Compute a match from the stored reference and the input layer at the current time.
PF_Err ComputeMatch(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], const char* refPath) {
    int rw, rh, rn;
    unsigned char* ref8 = stbi_load(refPath, &rw, &rh, &rn, 3);
    if (!ref8) {
        setError(out_data, "LookMatch: could not read the reference image (PNG, JPEG, TGA or BMP).");
        return PF_Err_NONE;
    }
    std::vector<lm::Vec3> refPx;
    size_t total = size_t(rw) * rh, step = std::max<size_t>(1, total / 250000);
    for (size_t i = 0; i < total; i += step) refPx.push_back({ref8[i * 3] / 255.0f, ref8[i * 3 + 1] / 255.0f, ref8[i * 3 + 2] / 255.0f});
    stbi_image_free(ref8);

    PF_ParamDef layer;
    AEFX_CLR_STRUCT(layer);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, P_INPUT_LAYER, in_data->current_time, in_data->time_step, in_data->time_scale, &layer);
    if (err) return err;
    PF_EffectWorld* world = &layer.u.ld;
    std::vector<lm::Vec3> srcPx;
    if (world->data) {
        std::vector<float> buf;
        Fmt fmt = worldFormat(in_data, world);
        worldToFloat(world, fmt, world->width, world->height, buf);
        lm::Engine e;
        e.setParams(readParams(params));
        size_t n = size_t(world->width) * world->height, st = std::max<size_t>(1, n / 250000);
        for (size_t i = 0; i < n; i += st) srcPx.push_back(e.neutralDisplay({buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2]}));
    }
    PF_CHECKIN_PARAM(in_data, &layer);
    if (srcPx.empty()) { setError(out_data, "LookMatch: no footage under the playhead to match."); return PF_Err_NONE; }

    lm::MatchOptions mo;
    mo.regionStrength = F(params, P_REGION_STRENGTH) / 100.0f;
    mo.skinProtect = params[P_SKIN_AUTO]->u.bd.value ? -1.0f : F(params, P_SKIN_PROTECT) / 100.0f;
    mo.lutSize = kMatchLutSize;
    lm::MatchResult m = lm::matchLook(refPx, srcPx, mo);

    SeqData* sd = lockSeq(in_data);
    if (!sd) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    float* f = sd->matchLut;
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r, f += 3) {
                const lm::Vec3& v = m.lut.at(r, g, b);
                f[0] = v.r; f[1] = v.g; f[2] = v.b;
            }
    std::strncpy(sd->referencePath, refPath, sizeof(sd->referencePath) - 1);
    sd->hasMatch = 1;
    sd->matchId = std::random_device{}() * 0x100000001ull + std::random_device{}();
    unlockSeq(in_data);
    out_data->out_flags |= PF_OutFlag_FORCE_RERENDER;
    return PF_Err_NONE;
}

PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_UserChangedParamExtra* extra) {
    switch (extra->param_index) {
        case P_LOAD_REF: {
            char path[1024] = {0};
            if (!LM_OpenImageDialog(path, sizeof path)) return PF_Err_NONE;
            return ComputeMatch(in_data, out_data, params, path);
        }
        case P_REMATCH: {
            SeqData* sd = lockSeq(in_data);
            std::string path = sd && sd->referencePath[0] ? sd->referencePath : "";
            if (sd) unlockSeq(in_data);
            if (path.empty()) { setError(out_data, "LookMatch: load a reference image first."); return PF_Err_NONE; }
            return ComputeMatch(in_data, out_data, params, path.c_str());
        }
        case P_EXPORT_LUT: {
            char path[1024] = {0};
            if (!LM_SaveCubeDialog(path, sizeof path)) return PF_Err_NONE;
            lm::Engine e;
            e.setParams(readParams(params));
            SeqData* sd = lockSeq(in_data);
            if (sd && sd->hasMatch) e.setMatchLut(lutFromSeq(sd));
            if (sd) unlockSeq(in_data);
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
    Fmt fmt = worldFormat(in_data, output);
    if (fmt == Fmt::Unsupported || worldFormat(in_data, input) != fmt) {
        setError(out_data, "LookMatch: unsupported pixel format.");
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    const int w = std::min(input->width, output->width), h = std::min(input->height, output->height);
    if (w <= 0 || h <= 0) return PF_Err_NONE;

    thread_local std::vector<float> buf;
    worldToFloat(input, fmt, w, h, buf);

    PoolEntry* pe = acquireEngine();
    pe->engine.setParams(readParams(params));
    if (SeqData* sd = lockSeq(in_data)) {
        uint64_t id = sd->hasMatch ? sd->matchId : 0;
        if (id != pe->matchId) {
            pe->engine.setMatchLut(sd->hasMatch ? lutFromSeq(sd) : nullptr);
            pe->matchId = id;
        }
        unlockSeq(in_data);
    }
    long frame = in_data->time_step ? long(in_data->current_time / in_data->time_step) : 0;
    lm::ImageView view{buf.data(), w, h, w * 4};
    pe->engine.render(view, view, frame);
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
            case PF_Cmd_SEQUENCE_SETUP: return SequenceSetup(in_data, out_data);
            case PF_Cmd_SEQUENCE_RESETUP: return SequenceResetup(in_data, out_data);
            case PF_Cmd_SEQUENCE_SETDOWN: return SequenceSetdown(in_data, out_data);
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
