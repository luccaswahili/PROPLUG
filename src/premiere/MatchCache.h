// Match cache: a match is a pure function of the reference stills, the frame it is built from and the
// match settings, so a re-match of the same clip with the same settings (matching a sequence again,
// undo and redo, another instance of the same clip) is read from refs/cache/<key>.lut and .json instead
// of being computed again. The key hashes the reference files (path, size, modification time), the
// frame as the match sees it (after the Input group, downsampled), every match setting and the match
// version, so any change misses. The newest entries are kept (pruneMatchCache).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <dirent.h>
#endif
#include "matcher.h"
#include "scene.h"

namespace lmpanel {

struct Fnv {  // FNV-1a, 64 bit
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }
    template <class T> void value(const T& v) { bytes(&v, sizeof v); }
    void text(const std::string& s) { bytes(s.data(), s.size()); value(s.size()); }
};

inline std::string matchCacheKey(const std::vector<std::string>& refPaths, const lm::Image& src,
                                 const lm::SceneMatchOptions& mo, uint32_t version, const std::string& build) {
    Fnv f;
    f.value(version);
    f.text(build);
    for (const std::string& p : refPaths) {
        f.text(p);
        struct stat st {};
        if (stat(p.c_str(), &st) == 0) { f.value(int64_t(st.st_size)); f.value(int64_t(st.st_mtime)); }
    }
    f.value(src.width); f.value(src.height);
    for (const lm::Vec3& v : src.px) {
        const uint16_t q[3] = {uint16_t(lm::clamp01(v.r) * 65535), uint16_t(lm::clamp01(v.g) * 65535), uint16_t(lm::clamp01(v.b) * 65535)};
        f.value(q);
    }
    f.value(mo.finish); f.value(mo.look.regionStrength); f.value(mo.look.skinProtect); f.value(mo.look.lutSize); f.value(mo.transfer);
    f.value(mo.exposure); f.value(mo.whiteBalance); f.value(mo.contrast); f.value(mo.lookStrength); f.value(mo.matchContrast);
    f.value(mo.method);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)f.h);
    return buf;
}

// keep the newest `keep` .lut entries (and their .json) of the cache directory `dir` (ends in '/')
inline void pruneMatchCache(const std::string& dir, size_t keep) {
#ifdef _WIN32
    (void)dir; (void)keep;  // (not yet on Windows)
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::pair<std::pair<long long, long long>, std::string>> files;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 5 || n.substr(n.size() - 4) != ".lut") continue;
        struct stat st {};
        if (stat((dir + n).c_str(), &st) != 0) continue;
#ifdef __APPLE__
        files.push_back({{(long long)st.st_mtimespec.tv_sec, (long long)st.st_mtimespec.tv_nsec}, n});
#else
        files.push_back({{(long long)st.st_mtim.tv_sec, (long long)st.st_mtim.tv_nsec}, n});
#endif
    }
    closedir(d);
    if (files.size() <= keep) return;
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + keep < files.size(); ++i) {
        const std::string stem = dir + files[i].second.substr(0, files[i].second.size() - 4);
        std::remove((stem + ".lut").c_str());
        std::remove((stem + ".json").c_str());
    }
#endif
}

}  // namespace lmpanel
