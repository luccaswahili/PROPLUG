// Engine sanity tests: run with `ctest` or ./engine_tests
#include <cmath>
#include <cstdio>
#include <random>
#include "engine.h"
#include "matcher.h"

using namespace lm;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

static std::vector<float> randomImage(int w, int h, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0, 1);
    std::vector<float> px(size_t(w) * h * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (i % 4 == 3) ? 1.0f : u(rng);
    return px;
}

int main() {
    const int w = 96, h = 64;

    // 1. with everything off the plugin must not change the picture (Rec.709)
    {
        auto src = randomImage(w, h, 1), dst = src;
        Engine e;
        e.render({src.data(), w, h, w * 4}, {dst.data(), w, h, w * 4}, 0);
        float maxErr = 0;
        for (size_t i = 0; i < src.size(); ++i) maxErr = std::max(maxErr, std::abs(src[i] - dst[i]));
        CHECK(maxErr < 0.5f / 255, "identity error %.5f", maxErr);
    }

    // 2. film keeps 18% grey neutral (apart from deliberate stock casts) and close to where it started, for every stock pair
    for (int n = 0; n < negativeStockCount(); ++n)
        for (int p = 1; p < printStockCount(); ++p) {
            FilmSettings fs; fs.negative = n; fs.print = p;
            FilmModel fm(fs);
            Vec3 g = fm.apply({0.18f, 0.18f, 0.18f});
            float spread = std::max({g.r, g.g, g.b}) - std::min({g.r, g.g, g.b});
            const float* so = negativeStock(n).speedOffset;
            bool intendedCast = so[0] != 0 || so[1] != 0 || so[2] != 0;  // e.g. Portrait 400's warm bias
            CHECK(spread < (intendedCast ? 0.03f : 0.01f), "%s / %s grey not neutral (%.3f %.3f %.3f)", negativeStock(n).name, printStock(p).name, g.r, g.g, g.b);
            CHECK(std::abs(g.g - 0.41f) < 0.05f, "%s / %s grey at %.3f", negativeStock(n).name, printStock(p).name, g.g);
            // monotonic: brighter scene light never gives a darker print
            float prev = -1;
            for (float x = 0.001f; x < 16; x *= 1.25f) {
                float y = luma709(fm.apply({x, x, x}));
                CHECK(y >= prev - 1e-4f, "%s / %s not monotonic at %.3f", negativeStock(n).name, printStock(p).name, x);
                prev = y;
            }
        }

    // 3. exported LUT reproduces the colour part of a render (no spatial effects)
    {
        Params p; p.filmStrength = 1; p.film.negative = 0; p.film.print = 1; p.fade = 0.1f; p.exposure = 0.3f; p.temperature = 0.2f;
        Engine e; e.setParams(p);
        Lut3D lut = e.bakeExportLut(65);
        auto src = randomImage(w, h, 2), dst = src;
        e.render({src.data(), w, h, w * 4}, {dst.data(), w, h, w * 4}, 0);
        double err = 0;
        for (size_t i = 0; i < src.size(); i += 4) {
            Vec3 l = lut.apply({src[i], src[i + 1], src[i + 2]});
            err += std::abs(l.r - dst[i]) + std::abs(l.g - dst[i + 1]) + std::abs(l.b - dst[i + 2]);
        }
        err /= (src.size() / 4) * 3;
        CHECK(err * 255 < 1.0, "export LUT mean error %.3f/255", err * 255);
    }

    // 4. matching an image to itself is (nearly) a no-op
    {
        auto img = randomImage(128, 128, 3);
        std::vector<Vec3> px;
        for (size_t i = 0; i < img.size(); i += 4) px.push_back({img[i] * 0.8f + 0.1f, img[i + 1] * 0.5f + 0.2f, img[i + 2] * 0.6f});
        MatchResult m = matchLook(px, px, MatchOptions{});
        double err = 0;
        for (const Vec3& c : px) { Vec3 o = m.lut.apply(c); err += std::abs(o.r - c.r) + std::abs(o.g - c.g) + std::abs(o.b - c.b); }
        err /= px.size() * 3;
        CHECK(err * 255 < 2.0, "self-match mean error %.3f/255", err * 255);
    }

    // 5. .cube round trip
    {
        Lut3D a(17);
        a.fill([](Vec3 c) { return Vec3(c.g, c.b, c.r); });
        const char* path = "lm_test_roundtrip.cube";
        CHECK(a.writeCube(path, "t"), "write failed");
        Lut3D b;
        std::string err;
        CHECK(b.readCube(path, &err), "read failed: %s", err.c_str());
        Vec3 v = b.apply({0.2f, 0.5f, 0.9f});
        CHECK(std::abs(v.r - 0.5f) < 1e-4f && std::abs(v.g - 0.9f) < 1e-4f && std::abs(v.b - 0.2f) < 1e-4f, "round trip mismatch");
        std::remove(path);
    }

    // 6. grain changes between frames, but is deterministic for a given frame
    {
        Params p; p.grain = 1;
        Engine e; e.setParams(p);
        std::vector<float> src(size_t(w) * h * 4, 0.5f), a = src, b = src, c = src;
        e.render({src.data(), w, h, w * 4}, {a.data(), w, h, w * 4}, 5);
        e.render({src.data(), w, h, w * 4}, {b.data(), w, h, w * 4}, 5);
        e.render({src.data(), w, h, w * 4}, {c.data(), w, h, w * 4}, 6);
        CHECK(a == b, "grain not deterministic");
        CHECK(a != c, "grain identical across frames");
    }

    std::printf(failures ? "%d check(s) failed\n" : "all engine tests passed\n", failures);
    return failures ? 1 : 0;
}
