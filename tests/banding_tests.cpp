// Contour rings on smooth gradients (skies), measured on the rendered output the way they look.
//
// A compressed 8- or 10-bit log sky is a staircase of flat steps one or two code values high. A grade
// that expands log to display multiplies each step 4-7x, and once a step reaches ~1/255 the flat
// plateaus show as concentric rings. The test renders such skies through the engine (the same
// per-pixel chain as the GPU kernel) and, along each column of the sky, measures how much of the
// gradient's change happens in visible jumps rather than smoothly:
//   step share: sum of the row-to-row changes larger than 0.5/255, over the sum of all changes
//               (after averaging 17 pixels across, so dither and noise don't count). Smooth ~0, rings ~1.
//   largest step: the 99th percentile of those changes, in /255.
// Frames: a synthetic S-Log3 sky quantised to 10 and 8 bits, plus any 16-bit PNG in tests/frames-sky
// (local only, git-ignored), whose top `sky` rows/columns are given in the file name: NAME-sky<W>x<H>.png.
//
//   banding_tests [--ref IMAGE] [--save DIR] [--no-deband]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>
#include "engine.h"
#include "matcher.h"
#include "scene.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace lm;

static int failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

struct Frame {
    std::string name;
    int w = 0, h = 0;
    std::vector<float> px;  // RGBA
    int skyW = 0, skyH = 0;
};

static Image toImage(const Frame& f, int maxWidth) {
    Image im;
    im.width = f.w; im.height = f.h;
    im.px.resize(size_t(f.w) * f.h);
    for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = {f.px[i * 4], f.px[i * 4 + 1], f.px[i * 4 + 2]};
    return downsample(im, maxWidth);
}

// A clear sky in S-Log3 code values (from the Marsa beach frame: blue, brighter and less saturated toward
// the horizon), quantised to `bits` as a camera codec stores it.
static Frame syntheticSky(int bits) {
    Frame f;
    f.name = "synthetic S-Log3 sky, " + std::to_string(bits) + "-bit";
    f.w = 1920; f.h = 1080; f.skyW = 1920; f.skyH = 760;
    f.px.resize(size_t(f.w) * f.h * 4);
    const float q = float((1 << bits) - 1);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            float* p = &f.px[(size_t(y) * f.w + x) * 4];
            float t, r, g, b;
            if (y < f.skyH) {
                // a gentle radial gradient around a point off the top left, like the sun side of a sky
                const float dx = (x - 300.0f) / 1920, dy = (y + 200.0f) / 1080;
                t = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / 1.1f);
                r = 0.500f + 0.058f * t; g = 0.526f + 0.046f * t; b = 0.582f + 0.024f * t;
            } else {  // ground: sand with texture
                const float n = std::sin(x * 0.37f) * std::sin(y * 0.29f) * 0.03f;
                r = 0.52f + n; g = 0.48f + n; b = 0.42f + n;
            }
            p[0] = std::round(r * q) / q; p[1] = std::round(g * q) / q; p[2] = std::round(b * q) / q; p[3] = 1;
        }
    return f;
}

static bool load16(const std::string& path, Frame& f) {
    int n;
    stbi_us* d = stbi_load_16(path.c_str(), &f.w, &f.h, &n, 4);
    if (!d) return false;
    f.px.resize(size_t(f.w) * f.h * 4);
    for (size_t i = 0; i < f.px.size(); ++i) f.px[i] = d[i] / 65535.0f;
    stbi_image_free(d);
    return true;
}

struct Steps { double share = 0, largest = 0; };

static Steps measure(const std::vector<float>& out, int w, int skyW, int skyH) {
    double total = 0, jumps = 0;
    std::vector<double> sizes;
    for (int x = 8; x + 8 < skyW; x += 3) {
        double prev[3] = {0, 0, 0};
        for (int y = 0; y < skyH; ++y) {
            double v[3] = {0, 0, 0};
            for (int dx = -8; dx <= 8; ++dx)
                for (int c = 0; c < 3; ++c) v[c] += out[(size_t(y) * w + x + dx) * 4 + c] / 17.0;
            if (y > 0)
                for (int c = 0; c < 3; ++c) {
                    const double d = std::abs(v[c] - prev[c]) * 255;
                    total += d;
                    if (d > 0.5) { jumps += d; sizes.push_back(d); }
                }
            std::copy(v, v + 3, prev);
        }
    }
    // the 99th percentile, so a bird or the edge of a cloud isn't a ring
    std::sort(sizes.begin(), sizes.end());
    const double largest = sizes.size() < 100 ? (sizes.empty() ? 0 : sizes.back()) : sizes[sizes.size() * 99 / 100];
    return {total > 0 ? jumps / total : 0, largest};
}

int main(int argc, char** argv) {
    std::string refPath, saveDir;
#ifdef LOOKMATCH_TEST_REF
    refPath = LOOKMATCH_TEST_REF;
#endif
    bool deband = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ref") && i + 1 < argc) refPath = argv[++i];
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) saveDir = argv[++i];
        else if (!std::strcmp(argv[i], "--no-deband")) deband = false;
    }
    Image ref;
    {
        int w, h, n;
        unsigned char* d = refPath.empty() ? nullptr : stbi_load(refPath.c_str(), &w, &h, &n, 3);
        if (d) {
            ref.width = w; ref.height = h;
            for (int i = 0; i < w * h; ++i) ref.px.push_back({d[i * 3] / 255.0f, d[i * 3 + 1] / 255.0f, d[i * 3 + 2] / 255.0f});
            stbi_image_free(d);
        } else {  // no reference still: a contrasty, saturated grade of the synthetic sky itself
            Frame s = syntheticSky(10);
            ref = toImage(s, 480);
            for (Vec3& v : ref.px) v = clamp01(encodeFromRec709(Transfer::Rec709, decodeToRec709(Transfer::SLog3, v) * 1.6f));
            std::printf("(no reference still at '%s': using a synthetic grade)\n", refPath.c_str());
        }
    }

    std::vector<Frame> frames = {syntheticSky(10), syntheticSky(8)};
    const std::string dir = std::string(LOOKMATCH_SOURCE_DIR) + "/tests/frames-sky";
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            int sw = 0, sh = 0;
            const size_t k = n.rfind("-sky");
            if (k == std::string::npos || std::sscanf(n.c_str() + k, "-sky%dx%d.png", &sw, &sh) != 2) continue;
            Frame f;
            if (!load16(dir + "/" + n, f)) continue;
            f.name = n; f.skyW = std::min(sw, f.w - 1); f.skyH = std::min(sh, f.h);
            frames.push_back(std::move(f));
        }
        closedir(d);
    }

    std::printf("%-58s %12s %12s\n", "frame", "step share", "largest");
    for (const Frame& f : frames) {
        SceneMatch m = matchScene(ref, toImage(f, 480), SceneMatchOptions{});
        Engine e;
        Params p;
        p.deband = deband ? 1.0f : 0.0f;
        e.setParams(p);
        e.setMatchLut(std::make_shared<Lut3D>(m.lut));
        std::vector<float> in = f.px, out(f.px.size());
        e.render({in.data(), f.w, f.h, f.w * 4}, {out.data(), f.w, f.h, f.w * 4}, 0);
        bool nan = false;
        for (float v : out) nan |= !std::isfinite(v);
        CHECK(!nan, "%s: NaN in the output", f.name.c_str());
        const Steps s = measure(out, f.w, f.skyW, f.skyH);
        std::printf("%-58s %11.1f%% %8.2f/255\n", f.name.c_str(), s.share * 100, s.largest);
        CHECK(s.share < 0.15 && s.largest < 1.0, "%s: contour rings in the sky (step share %.0f%%, largest step %.2f/255)",
              f.name.c_str(), s.share * 100, s.largest);
        if (!saveDir.empty()) {
            std::vector<unsigned char> b(size_t(f.w) * f.h * 3);
            for (size_t i = 0; i < size_t(f.w) * f.h; ++i)
                for (int c = 0; c < 3; ++c) b[i * 3 + c] = (unsigned char)(clamp01(out[i * 4 + c]) * 255 + 0.5f);
            std::string fn = saveDir + "/" + f.name.substr(0, f.name.find('.')) + (deband ? "" : "-nodeband") + ".png";
            for (char& ch : fn) if (ch == ' ' || ch == ',') ch = '_';
            stbi_write_png(fn.c_str(), f.w, f.h, 3, b.data(), f.w * 3);
        }
    }
    // detail: deband must not smooth texture. On real log frames (skin, fabric, foliage, sand), compare
    // the high-pass energy (pixel minus its 5x5 mean) of the output with and without deband.
    const std::string logDir = std::string(LOOKMATCH_SOURCE_DIR) + "/tests/frames-log";
    double worstKept = 1;
    std::string worstName;
    if (DIR* d = opendir(logDir.c_str())) {
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) if (std::strstr(e->d_name, ".png")) names.push_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) {
            int w, h, c;
            stbi_us* px = stbi_load_16((logDir + "/" + n).c_str(), &w, &h, &c, 4);
            if (!px) continue;
            Frame f;
            f.w = w; f.h = h;
            f.px.resize(size_t(w) * h * 4);
            for (size_t i = 0; i < f.px.size(); ++i) f.px[i] = px[i] / 65535.0f;
            stbi_image_free(px);
            SceneMatch m = matchScene(ref, toImage(f, 480), SceneMatchOptions{});
            double hp[2] = {0, 0};
            for (int on = 0; on < 2; ++on) {
                Engine e;
                Params p;
                p.deband = float(on);
                e.setParams(p);
                e.setMatchLut(std::make_shared<Lut3D>(m.lut));
                std::vector<float> in = f.px, out(f.px.size());
                e.render({in.data(), w, h, w * 4}, {out.data(), w, h, w * 4}, 0);
                for (int y = 2; y + 2 < h; y += 2)
                    for (int x = 2; x + 2 < w; x += 2)
                        for (int ch = 0; ch < 3; ++ch) {
                            double mean = 0;
                            for (int dy = -2; dy <= 2; ++dy)
                                for (int dx = -2; dx <= 2; ++dx) mean += out[(size_t(y + dy) * w + x + dx) * 4 + ch];
                            const double v = out[(size_t(y) * w + x) * 4 + ch] - mean / 25;
                            hp[on] += v * v;
                        }
            }
            const double kept = std::sqrt(hp[1] / std::max(1e-12, hp[0]));
            if (kept < worstKept) { worstKept = kept; worstName = n; }
        }
    }
    if (!worstName.empty()) {
        std::printf("detail kept by deband (worst of the log frames): %.1f%% (%s)\n", worstKept * 100, worstName.c_str());
        CHECK(worstKept > 0.93, "deband smooths texture: %.0f%% of the high-pass detail kept on %s", worstKept * 100, worstName.c_str());
    }
    if (failures) std::printf("%d check(s) failed\n", failures);
    else std::printf("all banding tests passed\n");
    return failures ? 1 : 0;
}
