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
//   banding_tests [--ref IMAGE] [--save DIR] [--no-deband] [--match-contrast F]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <random>
#include <string>
#include <vector>
#include "color.h"
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
    float matchContrast = 1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ref") && i + 1 < argc) refPath = argv[++i];
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) saveDir = argv[++i];
        else if (!std::strcmp(argv[i], "--no-deband")) deband = false;
        else if (!std::strcmp(argv[i], "--match-contrast") && i + 1 < argc) matchContrast = float(std::atof(argv[++i]));
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
        SceneMatchOptions so;
        so.matchContrast = matchContrast;
        SceneMatch m = matchScene(ref, toImage(f, 480), so);
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
        // with next to no steps left, "largest" is a bird or a cloud edge, not a ring
        CHECK(s.share < 0.15 && (s.largest < 1.0 || s.share < 0.03), "%s: contour rings in the sky (step share %.0f%%, largest step %.2f/255)",
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
    // noise: on shaded skin, the deband must lower the source's noise, not add to it (it once made chroma
    // noise 1.7x worse: uneven neighbour sets on a gradient, per-channel dither). A skin-coloured S-Log3-like
    // patch with a shading gradient, bright and in shadow, plus Gaussian chroma and luma noise of about what
    // a 10-bit camera file carries. Input and output are measured the same way, in OKLab of the display
    // image: noise = RMS of neighbouring-pixel differences / sqrt 2.
    for (const float sigma : {0.0008f, 0.0003f}) {  // the second is about what the Artlist ProRes clips carry
        std::mt19937 rng(7);
        std::normal_distribution<float> gauss(0, 1);
        Frame f;
        f.name = "skin noise"; f.w = 640; f.h = 360;
        f.px.resize(size_t(f.w) * f.h * 4);
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                float* p = &f.px[(size_t(y) * f.w + x) * 4];
                const float shade = (x < f.w / 2 ? 0.50f : 0.38f) + 0.04f * y / f.h;  // lit | shadow, with a gradient
                const float n = sigma * gauss(rng);
                p[0] = shade + 0.022f + n + sigma * gauss(rng);
                p[1] = shade + n + sigma * gauss(rng);
                p[2] = shade - 0.016f + n + sigma * gauss(rng);
                p[3] = 1;
            }
        auto chroma = [&](const std::vector<float>& px, int x0, int x1) {
            double s = 0; long n = 0;
            for (int y = 20; y < f.h - 20; ++y)
                for (int x = x0; x + 1 < x1; ++x) {
                    const float* a = &px[(size_t(y) * f.w + x) * 4];
                    const Vec3 la = rgbToOklab(clamp01(Vec3(a[0], a[1], a[2]))), lb = rgbToOklab(clamp01(Vec3(a[4], a[5], a[6])));
                    s += (la.g - lb.g) * (la.g - lb.g) + (la.b - lb.b) * (la.b - lb.b); ++n;
                }
            return std::sqrt(s / n / 2);
        };
        SceneMatchOptions so;
        so.transfer = int(Transfer::SLog3);
        SceneMatch m = matchScene(ref, toImage(f, 480), so);
        auto render = [&](float db) {
            Engine e;
            Params p;
            p.deband = db;
            e.setParams(p);
            e.setMatchLut(std::make_shared<Lut3D>(m.lut));
            std::vector<float> in = f.px, out(f.px.size());
            e.render({in.data(), f.w, f.h, f.w * 4}, {out.data(), f.w, f.h, f.w * 4}, 0);
            return out;
        };
        std::vector<float> dec = f.px;
        for (size_t i = 0; i < dec.size(); i += 4) {
            Vec3 l = decodeToRec709(Transfer::SLog3, Vec3(dec[i], dec[i + 1], dec[i + 2]));
            dec[i] = rec709Encode(clamp01(l.r)); dec[i + 1] = rec709Encode(clamp01(l.g)); dec[i + 2] = rec709Encode(clamp01(l.b));
        }
        const std::vector<float> on = render(1), off = render(0);
        for (int side = 0; side < 2; ++side) {
            const int x0 = side ? f.w / 2 + 20 : 20, x1 = side ? f.w - 20 : f.w / 2 - 20;
            const double in = chroma(dec, x0, x1), o0 = chroma(off, x0, x1), o1 = chroma(on, x0, x1);
            std::printf("skin chroma noise %.4f, %s: decoded %.5f, matched without deband %.5f, with deband %.5f (out/in %.2f)\n",
                        sigma, side ? "shadow" : "lit", in, o0, o1, o1 / in);
            CHECK(o1 < o0, "deband adds chroma noise on %s skin (%.5f vs %.5f)", side ? "shadow" : "lit", o1, o0);
            CHECK(o1 / in < 0.75, "%s skin: chroma noise %.2fx of the decoded clip (noise %.4f)", side ? "shadow" : "lit", o1 / in, sigma);
        }
    }

    // skin texture: no plastic skin. A skin patch with pore-scale luma texture (a few pixels across, at the
    // strength of the bedroom clip's lit skin, band RMS about 0.006) plus chroma noise, through the deband
    // alone: a stress case, random texture with no shading behind it, must keep at least 80% (real lit skin keeps
    // 97-99% of its pore-scale detail), and chroma noise must not rise. Texture much fainter than this
    // (around the size of grain) is reduced: it can't be told from grain and a compressed sky's steps.
    {
        const int w = 512, h = 512;
        std::mt19937 rng(11);
        std::normal_distribution<float> gauss(0, 1);
        std::vector<float> tex(size_t(w) * h), px(size_t(w) * h * 4, 1.0f);
        for (float& v : tex) v = gauss(rng);
        auto blur = [&](const std::vector<float>& in, int r) {
            std::vector<float> o(in.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    float t = 0; int c = 0;
                    for (int j = std::max(0, y - r); j <= std::min(h - 1, y + r); ++j)
                        for (int i = std::max(0, x - r); i <= std::min(w - 1, x + r); ++i) { t += in[size_t(j) * w + i]; ++c; }
                    o[size_t(y) * w + x] = t / c;
                }
            return o;
        };
        std::vector<float> pores = blur(tex, 2);
        double rms = 0;
        for (float v : pores) rms += v * v;
        rms = std::sqrt(rms / pores.size());
        for (size_t i = 0; i < pores.size(); ++i) {
            const float t = pores[i] / float(rms) * 0.0065f + 0.5f + 0.03f * float(i / w) / h;  // texture on a soft shade
            px[i * 4] = t + 0.022f + 0.0003f * gauss(rng);
            px[i * 4 + 1] = t + 0.0003f * gauss(rng);
            px[i * 4 + 2] = t - 0.016f + 0.0003f * gauss(rng);
        }
        const DebandParams db = debandParams(1, 2160);  // as on a 4K frame
        std::vector<float> lumaIn(pores.size()), lumaOut(pores.size());
        double cIn = 0, cOut = 0;
        std::vector<float> chromaIn(pores.size()), chromaOut(pores.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const Vec3 o = debandAt(px.data(), w, h, w * 4, x, y, db);
                lumaIn[i] = (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3;
                lumaOut[i] = (o.r + o.g + o.b) / 3;
                chromaIn[i] = px[i * 4] - px[i * 4 + 1];
                chromaOut[i] = o.r - o.g;
            }
        auto band = [&](const std::vector<float>& v) {
            const std::vector<float> a = blur(v, 1), b = blur(v, 7);
            double e = 0; long n = 0;
            for (int y = 16; y < h - 16; ++y)
                for (int x = 16; x < w - 16; ++x) { const double d = a[size_t(y) * w + x] - b[size_t(y) * w + x]; e += d * d; ++n; }
            return std::sqrt(e / n);
        };
        const double kept = band(lumaOut) / band(lumaIn);
        for (int y = 16; y < h - 16; ++y)
            for (int x = 16; x + 1 < w - 16; ++x) {
                const size_t i = size_t(y) * w + x;
                cIn += (chromaIn[i] - chromaIn[i + 1]) * (chromaIn[i] - chromaIn[i + 1]);
                cOut += (chromaOut[i] - chromaOut[i + 1]) * (chromaOut[i] - chromaOut[i + 1]);
            }
        const double ratio = std::sqrt(cOut / cIn);
        std::printf("skin texture through the deband: pore-scale luma kept %.1f%%, chroma noise ratio %.2f\n", kept * 100, ratio);
        CHECK(kept > 0.80 && kept < 1.05, "deband changes skin texture: %.1f%% kept", kept * 100);
        CHECK(ratio <= 1.0, "deband raises chroma noise on textured skin (%.2f)", ratio);
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
        // the high-pass energy includes the source's grain, which the near ring is meant to take out
        CHECK(worstKept > 0.83, "deband smooths texture: %.0f%% of the high-pass detail kept on %s", worstKept * 100, worstName.c_str());
    }
    if (failures) std::printf("%d check(s) failed\n", failures);
    else std::printf("all banding tests passed\n");
    return failures ? 1 : 0;
}
