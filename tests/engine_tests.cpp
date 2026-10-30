// Engine sanity tests: run with `ctest` or ./engine_tests
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include "engine.h"
#include "matcher.h"
#include "scene.h"
#include "../src/premiere/MatchCache.h"
#include <unistd.h>
#include <sys/stat.h>

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

    // 7. the fast colour path (one baked LUT) matches the full scene-linear path, for log input,
    //    film, fade, exposure and white balance; a negligible softness forces the full path.
    //    (Rec.709 uses one baked LUT; log input decodes per pixel in the same pass.)
    {
        std::vector<float> src(size_t(w) * h * 4);
        for (int i = 0; i < w * h; ++i) {
            src[i * 4] = 0.94f * float(i % 97) / 96; src[i * 4 + 1] = 0.94f * float(i % 61) / 60;
            src[i * 4 + 2] = 0.94f * float(i % 43) / 42; src[i * 4 + 3] = 1;
        }
        Params p;
        p.input = InputSpace::SonySLog3; p.exposure = 0.4f; p.temperature = 0.2f; p.tint = -0.1f;
        p.filmStrength = 0.8f; p.film.negative = 1; p.film.print = 2; p.fade = 0.1f; p.vignette = 0.3f;
        Params slow = p; slow.softness = 1e-6f;
        Engine ef, es; ef.setParams(p); es.setParams(slow);
        std::vector<float> a(src.size()), b(src.size());
        ef.render({src.data(), w, h, w * 4}, {a.data(), w, h, w * 4}, 0);
        es.render({src.data(), w, h, w * 4}, {b.data(), w, h, w * 4}, 0);
        double sum = 0; float worst = 0;
        for (size_t i = 0; i < a.size(); ++i) { float d = std::abs(a[i] - b[i]); sum += d; worst = std::max(worst, d); }
        CHECK(sum / a.size() * 255 < 0.5 && worst * 255 < 4, "fast path (log) differs: mean %.3f max %.3f /255", sum / a.size() * 255, worst * 255);
        // and the baked Rec.709 path
        Params r = p; r.input = InputSpace::Rec709;
        Params rs = r; rs.softness = 1e-6f;
        Engine rf, rsl; rf.setParams(r); rsl.setParams(rs);
        rf.render({src.data(), w, h, w * 4}, {a.data(), w, h, w * 4}, 0);
        rsl.render({src.data(), w, h, w * 4}, {b.data(), w, h, w * 4}, 0);
        sum = 0; worst = 0;
        for (size_t i = 0; i < a.size(); ++i) { float d = std::abs(a[i] - b[i]); sum += d; worst = std::max(worst, d); }
        // both paths interpolate a 65^3 LUT (code space vs shaped linear), so allow a few levels at worst
        CHECK(sum / a.size() * 255 < 0.25 && worst * 255 < 8, "fast path (709) differs: mean %.3f max %.3f /255", sum / a.size() * 255, worst * 255);
    }

    // 8. several host threads rendering at once (as Premiere does) give the same pixels as one at a time
    {
        Params p; p.filmStrength = 1; p.halation = 0.5f; p.bloom = 0.3f; p.grain = 0.6f; p.softness = 0.2f;
        auto src = randomImage(w, h, 3);
        std::vector<float> ref(src.size());
        { Engine e; e.setParams(p); e.render({src.data(), w, h, w * 4}, {ref.data(), w, h, w * 4}, 4); }
        std::vector<std::vector<float>> outs(6, std::vector<float>(src.size()));
        std::vector<std::thread> threads;
        for (int t = 0; t < 6; ++t)
            threads.emplace_back([&, t] {
                Engine e; e.setParams(p);
                for (int k = 0; k < 5; ++k) e.render({src.data(), w, h, w * 4}, {outs[t].data(), w, h, w * 4}, 4);
            });
        for (auto& th : threads) th.join();
        for (int t = 0; t < 6; ++t) CHECK(outs[t] == ref, "thread %d rendered different pixels", t);
    }

    // 9. source analysis: every transfer curve round-trips, CCT/tint invert, gamut matrices are sane
    for (int t = 0; t < kTransferCount; ++t) {
        float worst = 0;
        for (float x : {0.0f, 0.001f, 0.01f, 0.05f, 0.18f, 0.5f, 1.0f, 4.0f}) {
            if (!isLog(Transfer(t)) && x > 1) continue;
            float back = transferDecode(Transfer(t), transferEncode(Transfer(t), x));
            worst = std::max(worst, std::abs(back - x) / std::max(x, 0.01f));
        }
        CHECK(worst < 1e-3f, "%s does not round-trip (%.5f)", transferName(Transfer(t)), worst);
        Vec3 w = gamutToRec709(Transfer(t)) * Vec3(1, 1, 1);
        CHECK(std::abs(w.r - 1) < 2e-3f && std::abs(w.g - 1) < 2e-3f && std::abs(w.b - 1) < 2e-3f,
              "%s gamut doesn't keep white (%.4f %.4f %.4f)", transferName(Transfer(t)), w.r, w.g, w.b);
    }
    {
        Mat3 awg = gamutToRec709(Transfer::LogC3);  // against ARRI's published AWG3 -> Rec.709 matrix
        CHECK(std::abs(awg.m[0][0] - 1.617523f) < 2e-3f && std::abs(awg.m[1][1] - 1.334613f) < 2e-3f, "AWG3 matrix off (%.4f %.4f)",
              awg.m[0][0], awg.m[1][1]);
        float cct, tint, x, y;
        xyToCctTint(kD65x, kD65y, cct, tint);
        CHECK(std::abs(cct - 6504) < 15 && std::abs(tint) < 0.05f, "D65 reads %.0f K / %.2f", cct, tint);
        for (float k : {3200.0f, 4300.0f, 5600.0f, 7500.0f})
            for (float tn : {-20.0f, 0.0f, 20.0f}) {
                cctTintToXy(k, tn, x, y);
                xyToCctTint(x, y, cct, tint);
                CHECK(std::abs(cct - k) < k * 0.003f && std::abs(tint - tn) < 0.1f, "%.0f K / %.0f reads back %.0f / %.1f", k, tn, cct, tint);
            }
        // a scene with a cast: the analysis finds the cast
        Image img;
        img.width = 160; img.height = 90;
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> u(0, 1);
        cctTintToXy(4000, 0, x, y);
        Mat3 cast = adaptation(kD65x, kD65y, x, y);
        for (int i = 0; i < img.width * img.height; ++i) {
            float l = 0.02f + 0.6f * u(rng) * u(rng);
            Vec3 c = i % 3 ? Vec3(l, l, l) : Vec3(l * 1.2f, l * 0.9f, l * 0.5f);  // greys and some warm surfaces
            Vec3 lin = cast * c;
            img.px.push_back({rec709Encode(lin.r), rec709Encode(lin.g), rec709Encode(lin.b)});
        }
        SceneAnalysis a = analyseScene(img);
        CHECK(a.transfer == Transfer::Rec709 && std::abs(a.cct - 4000) < 250, "cast scene reads %s %.0f K", transferShortName(a.transfer), a.cct);
        // the same scene encoded in S-Log3 is detected as log
        Image logImg = img;
        for (Vec3& c : logImg.px) c = encodeFromRec709(Transfer::SLog3, Vec3(rec709Decode(c.r), rec709Decode(c.g), rec709Decode(c.b)));
        CHECK(isLog(detectTransfer(logImg).transfer), "S-Log3 frame not detected as log (%s)", detectTransfer(logImg).why.c_str());
    }

    // match cache: the key changes with every input the match depends on, and pruning keeps the newest
    {
        Image src;
        src.width = 8; src.height = 4;
        for (int i = 0; i < 32; ++i) src.px.push_back(Vec3(i / 32.0f, 0.5f, 1 - i / 32.0f));
        SceneMatchOptions mo;
        const std::vector<std::string> refs = {"/nonexistent/ref.png"};
        const std::string k0 = lmpanel::matchCacheKey(refs, src, mo, 2, "build");
        CHECK(k0 == lmpanel::matchCacheKey(refs, src, mo, 2, "build"), "cache key not stable");
        Image src2 = src; src2.px[5].g += 0.01f;
        SceneMatchOptions mo2 = mo; mo2.matchContrast = 0.5f;
        SceneMatchOptions mo3 = mo; mo3.transfer = 7;
        const std::string ks[] = {lmpanel::matchCacheKey(refs, src2, mo, 2, "build"), lmpanel::matchCacheKey(refs, src, mo2, 2, "build"),
                                  lmpanel::matchCacheKey(refs, src, mo3, 2, "build"), lmpanel::matchCacheKey({"/other.png"}, src, mo, 2, "build"),
                                  lmpanel::matchCacheKey(refs, src, mo, 3, "build"), lmpanel::matchCacheKey(refs, src, mo, 2, "build2"),
                                  lmpanel::matchCacheKey({refs[0], "/b.png"}, src, mo, 2, "build")};
        for (const std::string& k : ks) CHECK(k != k0, "cache key ignores an input");
        char tmpl[] = "/tmp/lmcacheXXXXXX";
        if (char* d = mkdtemp(tmpl)) {
            const std::string dir = std::string(d) + "/";
            for (int i = 0; i < 5; ++i) {
                for (const char* ext : {".lut", ".json"})
                    if (FILE* f = std::fopen((dir + "k" + std::to_string(i) + ext).c_str(), "wb")) { std::fputs("x", f); std::fclose(f); }
                usleep(20000);
            }
            lmpanel::pruneMatchCache(dir, 3);
            auto exists = [&](const std::string& n) { struct stat st {}; return stat((dir + n).c_str(), &st) == 0; };
            CHECK(!exists("k0.lut") && !exists("k1.lut") && !exists("k1.json"), "prune kept old cache entries");
            CHECK(exists("k2.lut") && exists("k4.lut") && exists("k4.json"), "prune removed new cache entries");
            for (int i = 0; i < 5; ++i) for (const char* ext : {".lut", ".json"}) std::remove((dir + "k" + std::to_string(i) + ext).c_str());
            rmdir(d);
        }
    }

    std::printf(failures ? "%d check(s) failed\n" : "all engine tests passed\n", failures);
    return failures ? 1 : 0;
}
