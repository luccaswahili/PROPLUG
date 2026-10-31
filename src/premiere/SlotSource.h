// Live grade sliders for panel matches (no host SDK dependencies, shared by the CPU effect and the
// Premiere GPU filter).
//
// Look Strength, Match Contrast, Finish and the other match settings are baked into the match LUT when it
// is built, so moving one of them means building the match again (about half a second). A rebuild must
// see the same frame as the first build, not whatever frame happens to be rendered next, so the frame the
// match was built from is kept as refs/<slot>.src: the downsampled picture exactly as the matcher reads it
// (Input group applied), plus the log curve detected from the full frame. refs/<slot>.key names the
// settings refs/<slot>.lut was built with, so the GPU filter knows when that file is stale.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include "scene.h"

namespace lmpanel {

struct SlotSource {
    lm::Image src;        // what the matcher reads: Input group applied, downsampled to 640 wide
    int detected = -1;    // the log curve detected from the full frame (lm::Transfer), or -1
    std::string logGuess; // its label for the analysis
};

constexpr uint32_t kSlotSourceMagic = 0x31535253;  // 'SRS1'

inline bool writeSlotSource(const std::string& file, const SlotSource& s) {
    const std::string tmp = file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const uint32_t hdr[5] = {kSlotSourceMagic, uint32_t(s.src.width), uint32_t(s.src.height), uint32_t(int32_t(s.detected)),
                             uint32_t(s.logGuess.size())};
    bool ok = std::fwrite(hdr, sizeof hdr, 1, f) == 1 &&
              std::fwrite(s.logGuess.data(), 1, s.logGuess.size(), f) == s.logGuess.size() &&
              std::fwrite(s.src.px.data(), sizeof(lm::Vec3), s.src.px.size(), f) == s.src.px.size();
    ok = std::fclose(f) == 0 && ok;
    if (ok) ok = std::rename(tmp.c_str(), file.c_str()) == 0;
    if (!ok) std::remove(tmp.c_str());
    return ok;
}

inline std::shared_ptr<const SlotSource> readSlotSource(const std::string& file) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return nullptr;
    uint32_t hdr[5] = {0, 0, 0, 0, 0};
    auto s = std::make_shared<SlotSource>();
    bool ok = std::fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == kSlotSourceMagic && hdr[1] > 0 && hdr[2] > 0 &&
              hdr[1] <= 8192 && hdr[2] <= 8192 && hdr[4] <= 256;
    if (ok) {
        s->src.width = int(hdr[1]);
        s->src.height = int(hdr[2]);
        s->detected = int(int32_t(hdr[3]));
        s->logGuess.resize(hdr[4]);
        s->src.px.resize(size_t(hdr[1]) * hdr[2]);
        ok = std::fread(&s->logGuess[0], 1, hdr[4], f) == hdr[4] &&
             std::fread(s->src.px.data(), sizeof(lm::Vec3), s->src.px.size(), f) == s->src.px.size();
    }
    std::fclose(f);
    return ok ? s : nullptr;
}

// The match settings a slot's LUT depends on, in Effect Controls units (percent, 1-based popups,
// checkbox 0/1). Values are compared at 0.1 precision, so float noise between the CPU and GPU readings
// of a parameter doesn't count as a change.
struct MatchSettings {
    float lookStrength = 100, matchContrast = 100, finish = 100;
    float normExposure = 100, normWhite = 100, normContrast = 100;
    float regionStrength = 100, skinAuto = 1, skinProtect = 30;
    float sourceFormat = 1, inputSpace = 1, exposure = 0, temperature = 0, tint = 0;
};

inline uint64_t settingsKey(const MatchSettings& m) {
    const float v[] = {m.lookStrength, m.matchContrast, m.finish, m.normExposure, m.normWhite, m.normContrast,
                       m.regionStrength, m.skinAuto, m.skinProtect, m.sourceFormat, m.inputSpace, m.exposure,
                       m.temperature, m.tint};
    uint64_t h = 1469598103934665603ULL;
    for (float x : v) {
        const int64_t q = int64_t(std::llround(double(x) * 10.0));
        for (int i = 0; i < 8; ++i) { h ^= uint64_t(q >> (i * 8)) & 0xff; h *= 1099511628211ULL; }
    }
    return h;
}

inline std::string keyHex(uint64_t k) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)k);
    return buf;
}

}  // namespace lmpanel
