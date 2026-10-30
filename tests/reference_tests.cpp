// Several reference stills of one look (SceneMatch's multi-still reference).
//
// Six frames are graded with one known look and used as reference stills; four other frames are matched.
// Differences are mean OKLab distances x 100 ("ΔEok", about the size of ΔE00 for small differences).
//   stability: how far the match moves with the choice of stills. With one still, the ΔEok between the
//              matches to two different stills of the same look (all pairs); with three, between two
//              disjoint sets of three. Three stills must move the match at least 20% less.
//   outlier:   one still of a different grade joins three: how far the match moves with the median
//              combine, against pooling all the stills' pixels into one reference.
//   black bars: the same picture with letterbox or pillarbox bars gives the same match.
//   shot-to-shot: clips matched to the hero clip's graded frame land closer to the hero in exposure and
//              white balance than clips matched to the look on their own.
//   reference_tests [--frames DIR]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>
#include "color.h"
#include "matcher.h"
#include "scene.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace lm;

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

static bool load(const std::string& path, Image& img) {
    int w, h, n;
    unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!d) return false;
    Image full;
    full.width = w; full.height = h;
    for (int i = 0; i < w * h; ++i) full.px.push_back({d[i * 3] / 255.0f, d[i * 3 + 1] / 255.0f, d[i * 3 + 2] / 255.0f});
    stbi_image_free(d);
    img = downsample(full, 320);
    return true;
}

// a grade: warm highlights, teal shadows, gentle S-curve, a little more saturation
static Vec3 gradeA(Vec3 c) {
    Vec3 l(rec709Decode(clamp01(c.r)), rec709Decode(clamp01(c.g)), rec709Decode(clamp01(c.b)));
    l = Vec3(l.r * 1.06f, l.g * 1.0f, l.b * 0.9f) + Vec3(0.0f, 0.006f, 0.012f);
    Vec3 d(rec709Encode(clamp01(l.r)), rec709Encode(clamp01(l.g)), rec709Encode(clamp01(l.b)));
    for (int k = 0; k < 3; ++k) { const float x = d[k]; d[k] = x + 0.35f * x * (1 - x) * (x - 0.5f) * 2; }
    Vec3 lab = rgbToOklab(clamp01(d));
    lab.g *= 1.15f; lab.b *= 1.15f;
    return clamp01(oklabToRgb(lab));
}
// a different grade (the odd still): cool, flat, desaturated
static Vec3 gradeB(Vec3 c) {
    Vec3 lab = rgbToOklab(clamp01(c));
    lab.r = 0.12f + lab.r * 0.8f;
    lab.g = lab.g * 0.7f - 0.01f; lab.b = lab.b * 0.7f - 0.03f;
    return clamp01(oklabToRgb(lab));
}
static Image apply(const Image& in, Vec3 (*f)(Vec3)) { Image o = in; for (Vec3& v : o.px) v = f(v); return o; }
static Image pooled(const std::vector<Image>& v) {  // all stills' pixels as one image
    Image o;
    o.width = v[0].width;
    for (const Image& i : v) o.px.insert(o.px.end(), i.px.begin(), i.px.begin() + size_t(o.width) * (i.px.size() / o.width));
    o.height = int(o.px.size() / o.width);
    return o;
}

// mean OKLab distance x 100
static double dE(const Image& a, const Image& b) {
    double s = 0;
    for (size_t i = 0; i < a.px.size(); ++i) {
        const Vec3 x = rgbToOklab(clamp01(a.px[i])), y = rgbToOklab(clamp01(b.px[i]));
        s += 100 * std::sqrt((x.r - y.r) * (x.r - y.r) + (x.g - y.g) * (x.g - y.g) + (x.b - y.b) * (x.b - y.b));
    }
    return s / std::max<size_t>(1, a.px.size());
}
static Image matched(const std::vector<Image>& refs, const Image& src) {
    SceneMatchOptions o;
    o.look.lutSize = 33;
    SceneMatch m = matchScene(refs, src, o);
    Image out = src;
    for (Vec3& v : out.px) v = m.lut.apply(v);
    return out;
}

int main(int argc, char** argv) {
    std::string dir = std::string(LOOKMATCH_SOURCE_DIR) + "/tests/frames";
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) dir = argv[++i];
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) if (std::strstr(e->d_name, ".png")) names.push_back(e->d_name);
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    std::vector<Image> frames;
    for (const std::string& n : names) {
        Image im;
        if (frames.size() < 10 && load(dir + "/" + n, im)) frames.push_back(im);
    }
    if (frames.size() < 10) {
        std::printf("(needs 10 frames in %s, found %zu: skipped)\n", dir.c_str(), frames.size());
        return 0;
    }
    std::vector<Image> stills;
    for (int i = 0; i < 6; ++i) stills.push_back(apply(frames[i], gradeA));
    const Image odd = apply(frames[0], gradeB);

    double single = 0, triple = 0, outMedian = 0, outPooled = 0;
    int ns = 0, nt = 0;
    for (int s = 6; s < 10; ++s) {
        const Image& src = frames[s];
        std::vector<Image> one;
        for (int i = 0; i < 6; ++i) one.push_back(matched({stills[i]}, src));
        for (int i = 0; i < 6; ++i)
            for (int j = i + 1; j < 6; ++j) { single += dE(one[i], one[j]); ++ns; }
        const std::vector<std::vector<int>> sets = {{0, 1, 2}, {3, 4, 5}, {0, 2, 4}, {1, 3, 5}};
        std::vector<Image> three;
        for (auto& st : sets) three.push_back(matched({stills[st[0]], stills[st[1]], stills[st[2]]}, src));
        triple += dE(three[0], three[1]) + dE(three[2], three[3]);
        nt += 2;
        // the odd still joins three
        const std::vector<Image> base = {stills[0], stills[1], stills[2]};
        std::vector<Image> withOdd = base;
        withOdd.push_back(odd);
        outMedian += dE(matched(base, src), matched(withOdd, src));
        outPooled += dE(matched({pooled(base)}, src), matched({pooled(withOdd)}, src));
    }
    single /= ns; triple /= nt; outMedian /= 4; outPooled /= 4;

    // shot-to-shot: frame 6 is the hero, matched to the look; frames 7-9 matched (a) each to the look on
    // its own, (b) to the hero's graded frame. How far each lands from the hero in exposure (stops) and
    // white balance (mired: 1e6/K), from the match's own analysis of its result.
    double expA = 0, expB = 0, wbA = 0, wbB = 0;
    {
        SceneMatchOptions o;
        o.look.lutSize = 33;
        const std::vector<Image> look = {stills[0], stills[1], stills[2]};
        const SceneMatch hero = matchScene(look, frames[6], o);
        Image heroOut = frames[6];
        for (Vec3& v : heroOut.px) v = hero.lut.apply(v);
        auto mired = [](float k) { return 1e6 / std::max(1000.0f, k); };
        for (int s = 7; s < 10; ++s) {
            const SceneMatch a = matchScene(look, frames[s], o), b = matchScene(heroOut, frames[s], o);
            expA += std::abs(a.result.exposure - hero.result.exposure);
            expB += std::abs(b.result.exposure - hero.result.exposure);
            wbA += std::abs(mired(a.result.cct) - mired(hero.result.cct));
            wbB += std::abs(mired(b.result.cct) - mired(hero.result.cct));
        }
        expA /= 3; expB /= 3; wbA /= 3; wbB /= 3;
    }
    std::printf("match moves with the choice of stills (mean ΔEok): one still %.2f, three stills %.2f\n", single, triple);
    std::printf("an odd still joining three moves the match: median combine %.2f, pooled pixels %.2f\n", outMedian, outPooled);
    std::printf("clips vs the hero after matching: to the look alone exposure %.2f stops, white balance %.1f mired; "
                "to the hero's graded frame %.2f stops, %.1f mired\n", expA, wbA, expB, wbB);
    CHECK(expB <= expA + 0.05 && wbB <= wbA + 2, "matching to the hero's graded frame doesn't bring clips closer to the hero");
    CHECK(triple < single * 0.8, "three stills don't steady the look (%.2f vs one still %.2f)", triple, single);
    CHECK(outMedian < outPooled, "the median combine is not more robust to an odd still (%.2f vs pooled %.2f)", outMedian, outPooled);
    // black bars aren't part of the picture: in the clip (the frame the host renders) or the reference
    {
        auto withBars = [](const Image& in, bool sides) {  // the same picture, with bars added around it
            const int bx = sides ? in.width / 16 : 0, by = sides ? 0 : in.height / 10;
            Image o;
            o.width = in.width + 2 * bx; o.height = in.height + 2 * by;
            o.px.assign(size_t(o.width) * o.height, Vec3(0, 0, 0));
            for (int y = 0; y < in.height; ++y)
                for (int x = 0; x < in.width; ++x) o.px[size_t(y + by) * o.width + x + bx] = in.at(x, y);
            return o;
        };
        const Image& src = frames[7];
        auto matchOf = [](const Image& ref, const Image& s) {
            SceneMatchOptions o;
            o.look.lutSize = 33;
            return matchScene(ref, s, o);
        };
        const SceneMatch base = matchOf(stills[0], src);
        const SceneMatch mClip = matchOf(stills[0], withBars(src, true)), mRef = matchOf(withBars(stills[0], false), src);
        Image clean = src, a = src, b = src;
        for (Vec3& v : clean.px) v = base.lut.apply(v);
        for (Vec3& v : a.px) v = mClip.lut.apply(v);
        for (Vec3& v : b.px) v = mRef.lut.apply(v);
        const double dClip = dE(clean, a), dRef = dE(clean, b);
        const Vec3 barOut = mClip.lut.apply(Vec3(0, 0, 0));
        std::printf("black bars change the match: in the clip %.2f, in the reference %.2f ΔEok; a bar renders at %.3f\n", dClip, dRef,
                    std::max({barOut.r, barOut.g, barOut.b}));
        CHECK(dClip < 0.5 && dRef < 0.5, "black bars change the match (%.2f, %.2f)", dClip, dRef);
        CHECK(std::max({barOut.r, barOut.g, barOut.b}) < 0.12f, "black bars don't stay black");
    }
    if (failures) std::printf("%d check(s) failed\n", failures);
    else std::printf("all reference tests passed\n");
    return failures ? 1 : 0;
}
