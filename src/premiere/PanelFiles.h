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

constexpr uint32_t kSlotLutMagic = 0x4C4D534C;  // 'LMSL'

// refs/<slot>.lut: the match the effect built for a panel Reference Slot.
inline std::shared_ptr<const lm::Lut3D> readSlotLut(const std::string& file) {
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

// The LUT stored in Match Data (16-bit per channel).
inline std::shared_ptr<lm::Lut3D> lutFromMatch(const MatchData* md) {
    auto lut = std::make_shared<lm::Lut3D>(kMatchLutSize);
    const uint16_t* v = md->lut;
    for (int b = 0; b < kMatchLutSize; ++b)
        for (int g = 0; g < kMatchLutSize; ++g)
            for (int r = 0; r < kMatchLutSize; ++r, v += 3)
                lut->at(r, g, b) = {v[0] / 65535.0f, v[1] / 65535.0f, v[2] / 65535.0f};
    return lut;
}

}  // namespace lmpanel
