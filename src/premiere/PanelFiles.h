// Files the LookMatch panel and effect exchange in <data>/refs (see docs/PANEL.md). No host SDK
// dependencies, so the CPU effect and the Premiere GPU filter share them.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "MatchData.h"
#include "lut3d.h"

namespace lmpanel {

inline std::string refsDir() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    return base ? std::string(base) + "\\PRO PLUGS\\LookMatch\\refs\\" : "";
#else
    const char* base = std::getenv("HOME");
    return base ? std::string(base) + "/Library/Application Support/PRO PLUGS/LookMatch/refs/" : "";
#endif
}

inline bool fileExists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

// First line of a small text file the panel wrote (a path), or "" if missing.
inline std::string fileLine(const std::string& file) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return "";
    char buf[1024] = {0};
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    std::string path(buf, n);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    return path;
}

constexpr uint32_t kSlotLutMagic = 0x4C4D534C;   // 'LMSL': LUT only (builds before 1.1)
constexpr uint32_t kSlotLutMagic2 = 0x324D534C;  // 'LSM2': shaper size, shaper, then the LUT

// refs/<slot>.lut: the match the effect built for a panel Reference Slot.
inline std::shared_ptr<const lm::Lut3D> readSlotLut(const std::string& file) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return nullptr;
    uint32_t hdr[2] = {0, 0};
    std::vector<float> v, shaper;
    bool ok = std::fread(hdr, sizeof hdr, 1, f) == 1 && (hdr[0] == kSlotLutMagic || hdr[0] == kSlotLutMagic2) &&
              (hdr[1] == uint32_t(kMatchLutSize) || hdr[1] == uint32_t(kSlotLutSize));
    const int size = ok ? int(hdr[1]) : 0;
    v.resize(size_t(size) * size * size * 3);
    if (ok && hdr[0] == kSlotLutMagic2) {
        uint32_t n = 0;
        ok = std::fread(&n, sizeof n, 1, f) == 1 && n <= 4096;
        shaper.resize(ok ? n : 0);
        ok = ok && std::fread(shaper.data(), sizeof(float), shaper.size(), f) == shaper.size();
    }
    ok = ok && std::fread(v.data(), sizeof(float), v.size(), f) == v.size();
    std::fclose(f);
    if (!ok) return nullptr;
    auto lut = std::make_shared<lm::Lut3D>(size);
    const float* q = v.data();
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r, q += 3) lut->at(r, g, b) = {q[0], q[1], q[2]};
    if (shaper.size() >= 2) lut->setShaper(std::move(shaper));
    return lut;
}

// Match Data as stored: version 1 (no shaper) or 2. Returns null for anything else.
inline bool matchDataUsable(const void* p, size_t size) {
    auto* md = static_cast<const MatchDataV1*>(p);
    if (!md || size < sizeof(MatchDataV1) || md->magic != kMatchMagic) return false;
    return md->version == 1 || (md->version == kMatchVersion && size >= sizeof(MatchData));
}

// The LUT stored in Match Data (16-bit per channel).
inline std::shared_ptr<lm::Lut3D> lutFromMatch(const MatchDataV1* md) {
    auto lut = std::make_shared<lm::Lut3D>(kMatchLutSize);
    const uint16_t* v = md->lut;
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r, v += 3)
                lut->at(r, g, b) = {v[0] / 65535.0f, v[1] / 65535.0f, v[2] / 65535.0f};
    if (md->version >= 2) {
        auto* m2 = static_cast<const MatchData*>(md);
        if (m2->shaperSize == kMatchShaperSize) {
            std::vector<float> s(kMatchShaperSize);
            for (int i = 0; i < kMatchShaperSize; ++i) s[i] = m2->shaper[i] / 65535.0f;
            lut->setShaper(std::move(s));
        }
    }
    return lut;
}

}  // namespace lmpanel
