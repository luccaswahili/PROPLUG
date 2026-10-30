// Consistency of the source-aware match: the same shot, graded or encoded differently, matched to the
// same reference must come out the same. For every frame and perturbation (exposure, white balance,
// curves, saturation, log encodings and combinations) the matched result is compared with the match
// of the untouched frame, in CIEDE2000.
//
//   consistency_tests [--frames DIR] [--ref IMAGE] [--max N] [--old] [--verbose] [--direct] [--exact-log]
//                     [--amounts EXPOSURE WHITE CONTRAST LOOK] [--detect] [--sheet OUT.png [--compare]]
// Frames default to tests/frames (see tools/extract_frames.swift), the reference to the
// LOOKMATCH_REF environment variable. Without frames it runs on synthetic scenes.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "engine.h"
#include "matcher.h"
#include "scene.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace lm;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

// ---------- images ----------

static bool loadImage(const std::string& path, Image& img, int maxWidth) {
    int w, h, n;
    unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!d) return false;
    Image full;
    full.width = w; full.height = h;
    full.px.resize(size_t(w) * h);
    for (size_t i = 0; i < full.px.size(); ++i) full.px[i] = {d[i * 3] / 255.0f, d[i * 3 + 1] / 255.0f, d[i * 3 + 2] / 255.0f};
    stbi_image_free(d);
    img = downsample(full, maxWidth);
    return true;
}

static void savePng(const std::string& path, const Image& img) {
    std::vector<unsigned char> b(img.px.size() * 3);
    for (size_t i = 0; i < img.px.size(); ++i)
        for (int c = 0; c < 3; ++c) b[i * 3 + c] = (unsigned char)(clamp01(img.px[i][c]) * 255 + 0.5f);
    stbi_write_png(path.c_str(), img.width, img.height, 3, b.data(), img.width * 3);
}

// Synthetic stand-ins when no real frames are around: sky, skin, foliage, a grey card, deep shadow.
static Image syntheticScene(int variant) {
    Image img;
    img.width = 320; img.height = 180;
    img.px.resize(size_t(img.width) * img.height);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            float u = x / 319.0f, v = y / 179.0f, n = 0.02f * std::sin(x * 0.7f + y * 1.3f + variant);
            Vec3 lin;
            if (v < 0.35f) lin = Vec3(0.35f, 0.5f, 0.85f) * (0.6f + 0.4f * u);
            else if (u < 0.3f) lin = Vec3(0.55f, 0.32f, 0.22f) * (0.35f + 0.5f * v);
            else if (u < 0.55f) lin = Vec3(0.08f, 0.16f, 0.05f) * (1 + v);
            else if (u < 0.7f) lin = Vec3(0.18f, 0.18f, 0.18f);
            else lin = Vec3(0.02f, 0.018f, 0.025f) * (1 + 3 * v * u);
            lin = lin * (variant ? 0.7f : 1.0f);
            img.px[size_t(y) * img.width + x] = {rec709Encode(lin.r + n * lin.r), rec709Encode(lin.g + n * lin.g), rec709Encode(lin.b + n * lin.b)};
        }
    return img;
}

// ---------- CIEDE2000 ----------

static Vec3 displayToLab(Vec3 c) {  // Rec.709 display code -> CIELAB (D65)
    Vec3 l(rec709Decode(clamp01(c.r)), rec709Decode(clamp01(c.g)), rec709Decode(clamp01(c.b)));
    Vec3 xyz = rec709ToXYZ(l);
    const float wx = 0.95047f, wy = 1.0f, wz = 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116; };
    float fx = f(xyz.r / wx), fy = f(xyz.g / wy), fz = f(xyz.b / wz);
    return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

static double deltaE00(Vec3 a, Vec3 b) {
    const double pi = 3.14159265358979, deg = pi / 180;
    double L1 = a.r, a1 = a.g, b1 = a.b, L2 = b.r, a2 = b.g, b2 = b.b;
    double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2), Cb = (C1 + C2) / 2;
    double G = 0.5 * (1 - std::sqrt(std::pow(Cb, 7) / (std::pow(Cb, 7) + std::pow(25.0, 7))));
    double a1p = (1 + G) * a1, a2p = (1 + G) * a2;
    double C1p = std::hypot(a1p, b1), C2p = std::hypot(a2p, b2);
    auto hue = [&](double bb, double ap) { if (bb == 0 && ap == 0) return 0.0; double h = std::atan2(bb, ap) / deg; return h < 0 ? h + 360 : h; };
    double h1p = hue(b1, a1p), h2p = hue(b2, a2p);
    double dLp = L2 - L1, dCp = C2p - C1p, dhp = 0;
    if (C1p * C2p != 0) { dhp = h2p - h1p; if (dhp > 180) dhp -= 360; else if (dhp < -180) dhp += 360; }
    double dHp = 2 * std::sqrt(C1p * C2p) * std::sin(dhp / 2 * deg);
    double Lbp = (L1 + L2) / 2, Cbp = (C1p + C2p) / 2, hbp = h1p + h2p;
    if (C1p * C2p != 0) { if (std::abs(h1p - h2p) > 180) hbp += h1p + h2p < 360 ? 360 : -360; hbp /= 2; }
    double T = 1 - 0.17 * std::cos((hbp - 30) * deg) + 0.24 * std::cos(2 * hbp * deg) + 0.32 * std::cos((3 * hbp + 6) * deg) -
               0.20 * std::cos((4 * hbp - 63) * deg);
    double dth = 30 * std::exp(-((hbp - 275) / 25) * ((hbp - 275) / 25));
    double Rc = 2 * std::sqrt(std::pow(Cbp, 7) / (std::pow(Cbp, 7) + std::pow(25.0, 7)));
    double Sl = 1 + 0.015 * (Lbp - 50) * (Lbp - 50) / std::sqrt(20 + (Lbp - 50) * (Lbp - 50));
    double Sc = 1 + 0.045 * Cbp, Sh = 1 + 0.015 * Cbp * T, Rt = -std::sin(2 * dth * deg) * Rc;
    double x = dLp / Sl, y = dCp / Sc, z = dHp / Sh;
    return std::sqrt(x * x + y * y + z * z + Rt * y * z);
}

struct DE { double mean = 0, p95 = 0, max = 0, dL = 0, dC = 0, dH = 0; };
static DE compare(const Image& a, const Image& b) {
    std::vector<double> d(a.px.size());
    double s = 0, sl = 0, sc = 0, sh = 0;
    for (size_t i = 0; i < a.px.size(); ++i) {
        Vec3 la = displayToLab(a.px[i]), lb = displayToLab(b.px[i]);
        d[i] = deltaE00(la, lb); s += d[i];
        double ca = std::hypot(la.g, la.b), cb = std::hypot(lb.g, lb.b);
        sl += std::abs(la.r - lb.r); sc += std::abs(ca - cb);
        sh += std::sqrt(std::max(0.0, (la.g - lb.g) * (la.g - lb.g) + (la.b - lb.b) * (la.b - lb.b) - (ca - cb) * (ca - cb)));
    }
    std::sort(d.begin(), d.end());
    const double n = double(d.size());
    return {s / n, d[d.size() * 95 / 100], d.back(), sl / n, sc / n, sh / n};
}

// ---------- skin ----------
// Soft skin mask of an untouched (display-referred) frame: warm hues around 52 deg in OKLab with some
// chroma, not black or blown (the same prior as the matcher's skinPrior).
static float skinPriorT(const Vec3& lab) {
    const float C = std::hypot(lab.g, lab.b), h = std::atan2(lab.b, lab.g) * 57.29578f;
    const float d = (std::fmod(h - 52.0f + 540.0f, 360.0f) - 180.0f) / 22.0f;
    auto ss = [](float a, float b, float v) { const float t = clamp01((v - a) / (b - a)); return t * t * (3 - 2 * t); };
    return std::exp(-d * d) * ss(0.006f, 0.02f, C) * (1 - ss(0.15f, 0.22f, C)) * ss(0.15f, 0.28f, lab.r) *
           (1 - ss(0.95f, 0.99f, lab.r));
}
struct SkinLook { double share = 0, L50 = 0, C50 = 0, h10 = 0, h50 = 0, h90 = 0; };
// skin of `img` where `mask` (weights) says skin: medians and the 10/90% hue spread, in OKLab
static SkinLook skinLook(const Image& img, const std::vector<float>& mask) {
    std::vector<std::pair<double, double>> L, C, H;
    double sw = 0;
    for (size_t i = 0; i < img.px.size(); ++i) {
        if (mask[i] < 0.3f) continue;
        const Vec3 lab = rgbToOklab(clamp01(img.px[i]));
        L.push_back({lab.r, mask[i]}); C.push_back({std::hypot(lab.g, lab.b), mask[i]});
        H.push_back({std::atan2(lab.b, lab.g) * 57.29578, mask[i]});
        sw += mask[i];
    }
    auto q = [](std::vector<std::pair<double, double>> v, double f) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        double t = 0, a = 0;
        for (auto& p : v) t += p.second;
        for (auto& p : v) { a += p.second; if (a >= t * f) return p.first; }
        return v.back().first;
    };
    SkinLook s;
    s.share = sw / std::max<size_t>(1, img.px.size());
    s.L50 = q(L, 0.5); s.C50 = q(C, 0.5); s.h10 = q(H, 0.1); s.h50 = q(H, 0.5); s.h90 = q(H, 0.9);
    return s;
}
// mean CIEDE2000 over skin pixels
static double skinDE(const Image& a, const Image& b, const std::vector<float>& mask) {
    double s = 0, w = 0;
    for (size_t i = 0; i < a.px.size(); ++i)
        if (mask[i] >= 0.3f) { s += mask[i] * deltaE00(displayToLab(a.px[i]), displayToLab(b.px[i])); w += mask[i]; }
    return w > 0 ? s / w : 0;
}

// ---------- perturbations (pixelwise, on Rec.709 display code values) ----------

using PixelFn = std::function<Vec3(Vec3)>;
static Vec3 toLin(Vec3 c) { return {rec709Decode(clamp01(c.r)), rec709Decode(clamp01(c.g)), rec709Decode(clamp01(c.b))}; }
static Vec3 toCode(Vec3 l) { return {rec709Encode(clamp01(l.r)), rec709Encode(clamp01(l.g)), rec709Encode(clamp01(l.b))}; }

static PixelFn exposure(float stops) { float g = std::exp2(stops); return [g](Vec3 c) { return toCode(toLin(c) * g); }; }
static PixelFn whiteBalance(float cct, float tint) {
    float x, y;
    cctTintToXy(cct, tint, x, y);
    Mat3 m = adaptation(kD65x, kD65y, x, y);  // neutrals take on the illuminant's colour
    return [m](Vec3 c) { return toCode(m * toLin(c)); };
}
static PixelFn curve(std::function<float(float)> f) { return [f](Vec3 c) { return Vec3(clamp01(f(c.r)), clamp01(f(c.g)), clamp01(f(c.b))); }; }
static PixelFn sCurve(float k) {  // k > 1 S-curve, < 1 inverse S, pivot 0.45
    return curve([k](float x) {
        float t = x < 0.45f ? 0.45f * std::pow(x / 0.45f, k) : 1 - 0.55f * std::pow((1 - x) / 0.55f, k);
        return t;
    });
}
static PixelFn liftGammaGain(Vec3 lift, Vec3 gamma, Vec3 gain) {
    return [=](Vec3 c) {
        Vec3 o;
        for (int i = 0; i < 3; ++i) o[i] = clamp01(gain[i] * std::pow(clamp01(lift[i] + c[i] * (1 - lift[i])), 1 / gamma[i]));
        return o;
    };
}
// Saturation as a scene-referred grade does it: in linear light, around each pixel's luminance.
static PixelFn saturation(float s) {
    return [s](Vec3 c) {
        Vec3 l = toLin(c);
        float y = luma709(l);
        return toCode(Vec3(y, y, y) + (l - Vec3(y, y, y)) * s);
    };
}
static PixelFn logEncode(Transfer t, float stops = 0, float cct = 0, float tint = 0) {
    Mat3 m{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    if (cct > 0) { float x, y; cctTintToXy(cct, tint, x, y); m = adaptation(kD65x, kD65y, x, y); }
    float g = std::exp2(stops);
    return [=](Vec3 c) { return encodeFromRec709(t, m * toLin(c) * g); };
}
static PixelFn chain(std::vector<PixelFn> fs) {
    return [fs](Vec3 c) { for (auto& f : fs) c = f(c); return c; };
}

struct Perturbation { std::string group, name; PixelFn fn; int trueTransfer = -1; };

static std::vector<Perturbation> perturbations() {
    std::vector<Perturbation> v;
    for (float e : {-2.0f, -1.0f, -0.5f, 0.5f, 1.0f, 2.0f}) {
        char n[32]; std::snprintf(n, sizeof n, "%+.1f stops", e);
        v.push_back({"exposure", n, exposure(e)});
    }
    for (auto [k, t] : std::vector<std::pair<float, float>>{{3200, 0}, {4300, 0}, {5500, 0}, {7500, 0}, {6504, 20}, {6504, -20},
                                                             {3200, 10}, {7500, -10}, {4300, -20}}) {
        char n[48]; std::snprintf(n, sizeof n, "%.0fK tint %+.0f", k, t);
        v.push_back({"white balance", n, whiteBalance(k, t)});
    }
    v.push_back({"curves", "S-curve", sCurve(1.6f)});
    v.push_back({"curves", "strong S-curve", sCurve(2.2f)});
    v.push_back({"curves", "inverse S", sCurve(0.65f)});
    v.push_back({"curves", "contrast +25%", curve([](float x) { return 0.45f + (x - 0.45f) * 1.25f; })});
    v.push_back({"curves", "lift +0.08", liftGammaGain({0.08f, 0.08f, 0.08f}, {1, 1, 1}, {1, 1, 1})});
    v.push_back({"curves", "gamma 0.8", liftGammaGain({0, 0, 0}, {0.8f, 0.8f, 0.8f}, {1, 1, 1})});
    v.push_back({"curves", "gamma 1.25", liftGammaGain({0, 0, 0}, {1.25f, 1.25f, 1.25f}, {1, 1, 1})});
    v.push_back({"curves", "gain 0.85", liftGammaGain({0, 0, 0}, {1, 1, 1}, {0.85f, 0.85f, 0.85f})});
    v.push_back({"curves", "colour LGG (warm gain, blue lift)", liftGammaGain({0.0f, 0.0f, 0.04f}, {1.0f, 0.95f, 1.0f}, {1.08f, 1.0f, 0.95f})});
    v.push_back({"curves", "faded (lift + gain)", liftGammaGain({0.1f, 0.1f, 0.1f}, {1, 1, 1}, {0.9f, 0.9f, 0.9f})});
    for (float s : {0.5f, 0.75f, 1.25f, 1.5f}) {
        char n[32]; std::snprintf(n, sizeof n, "saturation %.2f", s);
        v.push_back({"saturation", n, saturation(s)});
    }
    for (Transfer t : {Transfer::SLog3, Transfer::LogC3, Transfer::LogC4, Transfer::CLog3, Transfer::VLog, Transfer::FLog,
                       Transfer::FLog2, Transfer::AppleLog, Transfer::BMDFilm5, Transfer::Log3G10, Transfer::DLog})
        v.push_back({"log", std::string("as ") + transferShortName(t), logEncode(t), int(t)});
    v.push_back({"combined", "-1 stop, 3200K, S-curve", chain({exposure(-1), whiteBalance(3200, 0), sCurve(1.6f)})});
    v.push_back({"combined", "+0.5 stop, 7500K tint -10, sat 0.7", chain({exposure(0.5f), whiteBalance(7500, -10), saturation(0.7f)})});
    v.push_back({"combined", "lift, gamma 1.2, 5500K, sat 1.3", chain({liftGammaGain({0.06f, 0.06f, 0.06f}, {1.2f, 1.2f, 1.2f}, {1, 1, 1}), whiteBalance(5500, 0), saturation(1.3f)})});
    v.push_back({"combined", "S-Log3 shot -1 stop at 4300K", logEncode(Transfer::SLog3, -1, 4300, 0), int(Transfer::SLog3)});
    v.push_back({"combined", "LogC3 shot +1 stop at 7500K tint +10", logEncode(Transfer::LogC3, 1, 7500, 10), int(Transfer::LogC3)});
    v.push_back({"combined", "V-Log shot -0.5 stop, tint -15", logEncode(Transfer::VLog, -0.5f, 6504, -15), int(Transfer::VLog)});
    v.push_back({"combined", "inverse S, gain 0.9, 4300K tint +10", chain({sCurve(0.65f), liftGammaGain({0, 0, 0}, {1, 1, 1}, {0.9f, 0.9f, 0.9f}), whiteBalance(4300, 10)})});
    return v;
}

static Image applyFn(const Image& in, const PixelFn& f) {
    Image o = in;
    for (Vec3& v : o.px) v = f(v);
    return o;
}
static Image applyLut(const Image& in, const Lut3D& lut) {
    Image o = in;
    for (Vec3& v : o.px) v = lut.apply(v);
    return o;
}
// Share of pixels with a channel at the top or bottom code, as a perturbation can clip a frame for good.
static float clippedShare(const Image& img) {
    size_t n = 0;
    for (const Vec3& c : img.px) {
        float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
        if (mx >= 0.999f || (mx <= 0.002f && mn <= 0.002f)) ++n;
    }
    return float(n) / img.px.size();
}

// The previous matcher (display LUT from the neutral display frame), for the before/after table.
static Lut3D oldMatch(const Image& ref, const Image& src) {
    std::vector<Vec3> r(ref.px.begin(), ref.px.end()), s(src.px.begin(), src.px.end());
    MatchOptions mo;
    return matchLook(r, s, mo).lut;
}

// ---------- content preservation ----------
// A match must not erase a scene's own colour: frames with strong legitimate colour (a green forest,
// a blue-hour sky, a red-lit stage) keep their relative hue relationships. The frame's colourful
// pixels are split into hue sectors; every sector holding 3% of them must keep its place in the hue
// circle relative to the others and stay distinguishably coloured.
struct Content { bool ok = true; std::string why; int sectors = 0; };
static Content checkContent(const Image& in, const Image& out) {
    const int S = 8;
    double cnt[S] = {}, ia[S] = {}, ib[S] = {}, oa[S] = {}, ob[S] = {}, oc[S] = {};
    size_t colourful = 0;
    for (size_t i = 0; i < in.px.size(); ++i) {
        Vec3 a = rgbToOklab(clamp01(in.px[i])), b = rgbToOklab(clamp01(out.px[i]));
        float C = std::hypot(a.g, a.b);
        if (C < 0.04f || a.r < 0.15f) continue;
        ++colourful;
        float h = std::atan2(a.b, a.g);
        int k = int(std::floor((h + 3.14159265f) / (6.2831853f / S))) % S;
        cnt[k] += 1; ia[k] += a.g / C; ib[k] += a.b / C; oa[k] += b.g; ob[k] += b.b; oc[k] += std::hypot(b.g, b.b);
    }
    Content r;
    std::vector<std::pair<float, float>> hues;  // (input hue, output hue) per big sector
    for (int k = 0; k < S; ++k) {
        if (colourful == 0 || cnt[k] < 0.03 * colourful) continue;
        ++r.sectors;
        float hin = std::atan2(float(ib[k]), float(ia[k])), hout = std::atan2(float(ob[k]), float(oa[k]));
        float cout = float(oc[k] / cnt[k]);
        if (cout < 0.02f) { r.ok = false; r.why += "sector " + std::to_string(k) + " lost its colour; "; }
        hues.push_back({hin, hout});
    }
    // pairwise: hue differences keep their sign and at least a third of their size
    for (size_t i = 0; i < hues.size(); ++i)
        for (size_t j = i + 1; j < hues.size(); ++j) {
            float din = std::remainder(hues[j].first - hues[i].first, 6.2831853f);
            float dout = std::remainder(hues[j].second - hues[i].second, 6.2831853f);
            if (std::abs(din) < 0.35f) continue;  // neighbouring sectors may merge
            const bool opposite = std::abs(din) > 2.6f;  // ~opposite hues: the sign of the difference is arbitrary
            if ((!opposite && din * dout <= 0) || std::abs(dout) < std::abs(din) / 3) {
                r.ok = false;
                char b[96];
                std::snprintf(b, sizeof b, "hues %.0f/%.0f deg apart became %.0f; ", din * 57.3f, din * 57.3f, dout * 57.3f);
                r.why += b;
            }
        }
    return r;
}

// Strongly coloured scenes (synthetic, as such frames aren't among the clips): forest, blue hour, red stage.
static Image colourScene(int kind) {
    Image img;
    img.width = 320; img.height = 180;
    img.px.resize(size_t(img.width) * img.height);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            float u = x / 319.0f, v = y / 179.0f, n = 0.03f * std::sin(x * 0.9f + y * 1.7f) * std::cos(x * 0.31f);
            Vec3 lin;
            if (kind == 0) {  // forest: greens, a brown path, a little sky
                lin = v < 0.15f ? Vec3(0.5f, 0.6f, 0.7f) : u > 0.4f && u < 0.6f && v > 0.6f ? Vec3(0.18f, 0.11f, 0.06f)
                                                         : Vec3(0.05f, 0.16f + 0.1f * v, 0.03f) * (0.6f + 0.8f * u);
            } else if (kind == 1) {  // blue hour: deep blue sky, orange street lights, dark buildings
                lin = v < 0.55f ? Vec3(0.03f, 0.06f, 0.22f) * (1.2f - v) : (int(u * 12) % 3 == 0 && v < 0.7f) ? Vec3(0.8f, 0.4f, 0.1f)
                                                                         : Vec3(0.02f, 0.02f, 0.03f) * (1 + v);
            } else {  // red stage: red wash, a magenta light, a performer in warm skin tones
                lin = u > 0.42f && u < 0.58f && v > 0.3f ? Vec3(0.45f, 0.25f, 0.18f) : v < 0.2f ? Vec3(0.5f, 0.05f, 0.4f)
                                                                                 : Vec3(0.35f, 0.02f, 0.03f) * (1.2f - 0.6f * v);
            }
            lin = lin * (1 + n);
            img.px[size_t(y) * img.width + x] = {rec709Encode(lin.r), rec709Encode(lin.g), rec709Encode(lin.b)};
        }
    return img;
}

// ---------- LUT quality: no banding, no NaN ----------

// NaNs anywhere in the cube, and the grey axis must never get darker as the input gets brighter.
static void checkLut(const Lut3D& lut, const std::string& what, double& worstJump) {
    int nans = 0;
    for (int i = 0; i < lut.size() * lut.size() * lut.size(); ++i) {
        const Vec3& v = lut.data()[i];
        if (!std::isfinite(v.r) || !std::isfinite(v.g) || !std::isfinite(v.b)) ++nans;
    }
    CHECK(nans == 0, "%s: %d NaN entries", what.c_str(), nans);
    const int N = 1024;
    float peakY = luma709(lut.apply(Vec3(0, 0, 0)));
    double drop = 0;  // largest fall of luminance below what a darker grey already reached
    double maxStep = 0;
    Vec3 prev = lut.apply(Vec3(0, 0, 0));
    for (int i = 1; i <= N; ++i) {
        Vec3 c = lut.apply(Vec3(1, 1, 1) * (float(i) / N));
        float y = luma709(c);
        drop = std::max(drop, double(peakY - y));
        peakY = std::max(peakY, y);
        maxStep = std::max(maxStep, double(std::max({std::abs(c.r - prev.r), std::abs(c.g - prev.g), std::abs(c.b - prev.b)})));
        prev = c;
    }
    worstJump = std::max(worstJump, maxStep * 255);
    CHECK(drop * 255 < 1.0, "%s: grey ramp goes darker by %.2f/255", what.c_str(), drop * 255);
    CHECK(maxStep * 255 < 6.0, "%s: grey ramp jumps %.2f/255 between 1/1024 steps", what.c_str(), maxStep * 255);
}

// Banding / posterisation on the picture itself: a smooth version of the frame (box blurred, so it is
// all gentle gradients of the frame's own colours) must come out smooth. A grade that stretches a
// narrow band of values (8-bit steps, compression blocks) shows as a large local gain.
static Image blurred(const Image& in, int r) {
    Image o = in, t = in;
    const int w = in.width, h = in.height;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Vec3 a; int c = 0;
            for (int k = -r; k <= r; ++k) { int xx = std::clamp(x + k, 0, w - 1); a = a + in.at(xx, y); ++c; }
            t.px[size_t(y) * w + x] = a * (1.0f / c);
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Vec3 a; int c = 0;
            for (int k = -r; k <= r; ++k) { int yy = std::clamp(y + k, 0, h - 1); a = a + t.at(x, yy); ++c; }
            o.px[size_t(y) * w + x] = a * (1.0f / c);
        }
    return o;
}
static double localGain(const Image& in, const Image& out) {  // 99.9th percentile of |d out| / |d in|
    std::vector<float> g;
    const int w = in.width, h = in.height;
    auto d = [](Vec3 a, Vec3 b) { return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)}); };
    for (int y = 0; y < h - 1; ++y)
        for (int x = 0; x < w - 1; ++x) {
            for (auto [dx, dy] : {std::pair<int, int>{1, 0}, {0, 1}}) {
                float di = d(in.at(x, y), in.at(x + dx, y + dy)), dout = d(out.at(x, y), out.at(x + dx, y + dy));
                if (di > 0.25f / 255) g.push_back(dout / std::max(di, 1.0f / 255));
            }
        }
    if (g.empty()) return 0;
    std::sort(g.begin(), g.end());
    return g[g.size() * 999 / 1000];
}

int main(int argc, char** argv) {
    std::string framesDir = LOOKMATCH_SOURCE_DIR "/tests/frames", refPath, sheet;
    bool runOld = false, verbose = false, exactLog = false, direct = false, detectOnly = false, compareOnly = false;
    int maxFrames = 1000;
    int method = 0;
    std::string dumpDir;
    float matchContrast = 1;
    float amounts[4] = {1, 1, 1, 1};
    if (const char* e = std::getenv("LOOKMATCH_REF")) refPath = e;
#ifdef LOOKMATCH_DEFAULT_REF
    if (refPath.empty()) refPath = LOOKMATCH_DEFAULT_REF;
#endif
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) framesDir = argv[++i];
        else if (!std::strcmp(argv[i], "--ref") && i + 1 < argc) refPath = argv[++i];
        else if (!std::strcmp(argv[i], "--old")) runOld = true;
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
        else if (!std::strcmp(argv[i], "--exact-log")) exactLog = true;   // log curve known (metadata)
        else if (!std::strcmp(argv[i], "--direct")) direct = true;        // skip the LUT (measures LUT error)
        else if (!std::strcmp(argv[i], "--detect")) detectOnly = true;
        else if (!std::strcmp(argv[i], "--compare")) compareOnly = true;  // with --sheet: before/after sheet only
        else if (!std::strcmp(argv[i], "--amounts") && i + 4 < argc) {  // exposure white contrast look
            amounts[0] = float(std::atof(argv[i + 1])); amounts[1] = float(std::atof(argv[i + 2]));
            amounts[2] = float(std::atof(argv[i + 3])); amounts[3] = float(std::atof(argv[i + 4])); i += 4;
        }
        else if (!std::strcmp(argv[i], "--max") && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--match-contrast") && i + 1 < argc) matchContrast = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];  // frames for an external matcher
        else if (!std::strcmp(argv[i], "--method") && i + 1 < argc) method = std::atoi(argv[++i]);  // SceneMatchOptions::method
        else if (!std::strcmp(argv[i], "--sheet") && i + 1 < argc) sheet = argv[++i];
    }
    std::vector<std::pair<std::string, Image>> frames;
    if (DIR* d = opendir(framesDir.c_str())) {
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && (n.substr(n.size() - 4) == ".png" || n.substr(n.size() - 4) == ".jpg")) names.push_back(n);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        if (int(names.size()) > maxFrames) {  // spread the subset over the clips
            std::vector<std::string> pick;
            for (int k = 0; k < maxFrames; ++k) pick.push_back(names[size_t(k) * names.size() / maxFrames]);
            names = pick;
        }
        for (auto& n : names) {
            Image img;
            if (loadImage(framesDir + "/" + n, img, 320)) frames.push_back({n, img});
        }
    }
    Image ref;
    if (refPath.empty() || !loadImage(refPath, ref, 480)) {
        std::printf("no reference image (set LOOKMATCH_REF or --ref): using a synthetic warm, contrasty reference\n");
        ref = applyFn(syntheticScene(0), chain({sCurve(1.5f), whiteBalance(4500, 5), saturation(1.3f)}));
    }
    if (frames.empty()) {
        std::printf("no frames in %s (tools/extract_frames.swift makes them): using synthetic scenes\n", framesDir.c_str());
        frames.push_back({"synthetic-0", syntheticScene(0)});
        frames.push_back({"synthetic-1", syntheticScene(1)});
    }
    std::printf("%zu frames, reference %dx%d\n", frames.size(), ref.width, ref.height);

    const auto perts = perturbations();
    struct Acc { std::vector<double> means, p95s; std::vector<bool> clipped; };
    std::map<std::string, Acc> byName, byNameOld;
    std::vector<std::string> order;
    for (auto& p : perts) order.push_back(p.name);
    double worstJump = 0, worstGain = 0;
    int detectedLog = 0, logCases = 0, falseLog = 0;
    std::vector<std::pair<Image, std::string>> sheetTiles;

    if (!sheet.empty() && compareOnly) {  // untouched frames only: input, new match, old match
        SceneMatchOptions so0;
        so0.method = method;
        so0.look.lutSize = 65;
        std::vector<Image> tiles;
        for (auto& [fname, F] : frames) {
            tiles.push_back(F);
            tiles.push_back(applyLut(F, matchScene(ref, F, so0).lut));
            tiles.push_back(applyLut(F, oldMatch(ref, F)));
        }
        const int tw = tiles[0].width, th = tiles[0].height, cols = 6, rows = int((tiles.size() + cols - 1) / cols);
        Image s2;
        s2.width = cols * tw; s2.height = rows * th;
        s2.px.assign(size_t(s2.width) * s2.height, Vec3(0.1f, 0.1f, 0.1f));
        for (size_t k = 0; k < tiles.size(); ++k)
            for (int y = 0; y < std::min(th, tiles[k].height); ++y)
                for (int x = 0; x < std::min(tw, tiles[k].width); ++x)
                    s2.px[size_t(int(k / cols) * th + y) * s2.width + int(k % cols) * tw + x] = tiles[k].at(x, y);
        savePng(sheet, s2);
        return 0;
    }
    if (detectOnly) {
        for (auto& [fname, F] : frames) {
            TransferGuess g = detectTransfer(F);
            std::printf("%-46s %-12s %.2f %s\n", fname.c_str(), "(untouched)", g.logScore, g.why.c_str());
            for (auto& p : perts) {
                TransferGuess q = detectTransfer(applyFn(F, p.fn));
                bool wrong = (p.group == "log") != (q.logScore >= 0.5f) && p.group != "combined";
                std::printf("  %-44s %-8s %.2f %s%s\n", p.name.c_str(), transferShortName(q.transfer), q.logScore, q.why.c_str(),
                            wrong ? "  <-- WRONG" : "");
            }
        }
        return 0;
    }
    SceneMatchOptions so;
    so.method = method;
    so.matchContrast = matchContrast;
    so.look.lutSize = 65;  // as the effect builds panel matches (kSlotLutSize)
    so.exposure = amounts[0]; so.whiteBalance = amounts[1]; so.contrast = amounts[2]; so.lookStrength = amounts[3];
    auto applyMatch = [&](const Image& in, const SceneMatch& m) {
        if (!direct) return applyLut(in, m.lut);
        Image o = in;
        for (Vec3& v : o.px) v = m.exact(v);
        return o;
    };
    int contentFails = 0, contentChecked = 0;
    {
        const char* names[] = {"synthetic forest", "synthetic blue hour", "synthetic red stage"};
        for (int k = 0; k < 3; ++k) {
            Image F = colourScene(k);
            Content c = checkContent(F, applyLut(F, matchScene(ref, F, so).lut));
            ++contentChecked;
            std::printf("  content %-22s %d hue sectors: %s%s\n", names[k], c.sectors, c.ok ? "kept" : "CHANGED ", c.why.c_str());
            CHECK(c.ok, "%s: the match did not keep the scene's colour relationships (%s)", names[k], c.why.c_str());
            if (!c.ok) ++contentFails;
        }
    }
    std::map<std::string, std::vector<double>> skinByGroup;
    int skinFrames = 0, skinFlags = 0;
    for (auto& [fname, F] : frames) {
        SceneMatch base = matchScene(ref, F, so);
        Image outBase = applyMatch(F, base);
        std::vector<float> skinMask(F.px.size());
        for (size_t i = 0; i < F.px.size(); ++i) skinMask[i] = skinPriorT(rgbToOklab(clamp01(F.px[i])));
        const SkinLook skIn = skinLook(F, skinMask), skOut = skinLook(outBase, skinMask);
        // more than 40% "skin" is a warm scene (sunlit concrete, sand), not a face: the hue prior can't
        // tell them apart, so such frames are reported but not judged as skin
        const bool hasSkin = skIn.share >= 0.03 && skIn.share <= 0.4;
        if (skIn.share > 0.4)
            std::printf("  skin %-42s %4.0f%%  warm scene, not judged (out C %.3f h %.0f)\n", fname.c_str(), skIn.share * 100, skOut.C50, skOut.h50);
        if (hasSkin) {
            ++skinFrames;
            // flags: ashy (too little chroma for skin), orange (too much, or pushed past red-orange),
            // split (one face spread over more than 35 deg of hue)
            std::string flags;
            if (skOut.C50 < 0.028) flags += " ashy";
            if (skOut.C50 > 0.10 || skOut.h50 < 30) flags += " orange";
            if (skOut.h90 - skOut.h10 > 35 && skOut.h90 - skOut.h10 > (skIn.h90 - skIn.h10) + 10) flags += " split";
            std::printf("  skin %-42s %4.0f%%  in L %.2f C %.3f h %3.0f (%3.0f..%3.0f)  out L %.2f C %.3f h %3.0f (%3.0f..%3.0f)%s\n",
                        fname.c_str(), skIn.share * 100, skIn.L50, skIn.C50, skIn.h50, skIn.h10, skIn.h90, skOut.L50, skOut.C50,
                        skOut.h50, skOut.h10, skOut.h90, flags.c_str());
            if (!flags.empty()) ++skinFlags;
            CHECK(flags.empty(), "%s: skin after the match is%s", fname.c_str(), flags.c_str());
        }
        checkLut(base.lut, fname + " (untouched)", worstJump);
        {
            Content c = checkContent(F, outBase);
            ++contentChecked;
            if (!c.ok) { ++contentFails; std::printf("  content %s: %s\n", fname.c_str(), c.why.c_str()); }
            CHECK(c.ok, "%s: the match did not keep the frame's colour relationships (%s)", fname.c_str(), c.why.c_str());
        }
        const Image smoothF = blurred(F, 4);
        auto bandingCheck = [&](const Image& smooth, const SceneMatch& m, const std::string& what) {
            Image ex = smooth;
            for (Vec3& v : ex.px) v = m.exact(v);
            const double gl = localGain(smooth, applyLut(smooth, m.lut)), ge = localGain(smooth, ex);
            worstGain = std::max(worstGain, gl / std::max(ge, 1.0));
            // the LUT must not add steps the transform itself doesn't have
            CHECK(gl <= 1.25 * ge + 1.5, "%s: local gain %.1f through the LUT vs %.1f exact (banding)", what.c_str(), gl, ge);
        };
        bandingCheck(smoothF, base, fname + " (untouched)");
        if (verbose) {
            Image ex = F;
            for (Vec3& v : ex.px) v = base.exact(v);
            DE lutErr = compare(applyLut(F, base.lut), ex);
            std::printf("    LUT vs exact on the untouched frame: mean %.2f p95 %.2f max %.2f\n", lutErr.mean, lutErr.p95, lutErr.max);
            const Vec3 dirs[] = {{1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}, {1, 0.6f, 0.45f}};
            for (const Vec3& d : dirs) {
                Vec3 prev = base.exact(Vec3(0, 0, 0));
                double worst = 0; float at = 0; Vec3 a0, a1;
                for (int i = 1; i <= 2048; ++i) {
                    Vec3 c = base.exact(d * (i / 2048.0f));
                    double st = std::max({std::abs(c.r - prev.r), std::abs(c.g - prev.g), std::abs(c.b - prev.b)});
                    if (st > worst) { worst = st; at = i / 2048.0f; a0 = prev; a1 = c; }
                    prev = c;
                }
                std::printf("      exact ramp (%.2f %.2f %.2f): worst step %.1f/255 at %.3f (%.3f %.3f %.3f -> %.3f %.3f %.3f)\n", d.r, d.g, d.b,
                            worst * 255, at, a0.r, a0.g, a0.b, a1.r, a1.g, a1.b);
            }
        }
        if (isLog(base.source.transfer)) ++falseLog;
        const float baseClip = clippedShare(F);
        Lut3D oldBaseLut;
        Image oldBase;
        if (runOld) { oldBaseLut = oldMatch(ref, F); oldBase = applyLut(F, oldBaseLut); }
        if (!sheet.empty() && sheetTiles.size() < 40) {
            sheetTiles.push_back({F, fname});
            sheetTiles.push_back({outBase, "matched"});
        }
        if (!dumpDir.empty()) {
            savePng(dumpDir + "/" + fname.substr(0, fname.size() - 4) + "__untouched.png", F);
            for (size_t k = 0; k < perts.size(); ++k) {
                Image Fp = applyFn(F, perts[k].fn);
                char nm[32];
                std::snprintf(nm, sizeof nm, "__p%02zu.png", k);
                savePng(dumpDir + "/" + fname.substr(0, fname.size() - 4) + nm, Fp);
            }
            if (FILE* f = std::fopen((dumpDir + "/perturbations.tsv").c_str(), "w")) {
                for (size_t k = 0; k < perts.size(); ++k)
                    std::fprintf(f, "%02zu\t%s\t%s\t%.4f\n", k, perts[k].group.c_str(), perts[k].name.c_str(), clippedShare(applyFn(F, perts[k].fn)) - baseClip);
                std::fclose(f);
            }
        }
        for (auto& p : perts) {
            Image Fp = applyFn(F, p.fn);
            SceneMatchOptions po = so;
            if (exactLog && p.trueTransfer >= 0) po.transfer = p.trueTransfer;
            SceneMatch m = matchScene(ref, Fp, po);
            Image out = applyMatch(Fp, m);
            DE de = compare(out, outBase);
            bool clipped = clippedShare(Fp) > baseClip + 0.01f;
            byName[p.name].means.push_back(de.mean);
            if (hasSkin && !clipped) skinByGroup[p.group].push_back(skinDE(out, outBase, skinMask));
            byName[p.name].p95s.push_back(de.p95);
            byName[p.name].clipped.push_back(clipped);
            if (p.group == "log") { ++logCases; if (isLog(m.source.transfer)) ++detectedLog; }
            if (p.group != "log" && p.group != "combined" && isLog(m.source.transfer)) ++falseLog;
            checkLut(m.lut, fname + " / " + p.name, worstJump);
            bandingCheck(applyFn(smoothF, p.fn), m, fname + " / " + p.name);
            if (verbose)
                std::printf("    %-38s dL %4.1f dC %4.1f dH %4.1f dE %5.2f/%5.2f  %-8s log %.2f exp %+.2f %5.0fK %+5.1f con %.1f sat %.3f | out exp %+.2f %5.0fK %+5.1f con %.1f\n",
                            p.name.c_str(), de.dL, de.dC, de.dH, de.mean, de.p95, transferShortName(m.source.transfer), m.source.logScore,
                            m.source.exposure, m.source.cct, m.source.tint, m.source.contrast, m.source.saturation,
                            m.result.exposure, m.result.cct, m.result.tint, m.result.contrast);
            if (runOld) {
                Image oldOut = applyLut(Fp, oldMatch(ref, Fp));
                DE od = compare(oldOut, oldBase);
                byNameOld[p.name].means.push_back(od.mean);
                byNameOld[p.name].p95s.push_back(od.p95);
            }
            if (!sheet.empty() && fname == frames.front().first && sheetTiles.size() < 120 &&
                (p.group == "combined" || p.name == "as S-Log3" || p.name == "-2.0 stops" || p.name == "3200K tint +0")) {
                sheetTiles.push_back({Fp, p.name});
                sheetTiles.push_back({out, "matched"});
            }
        }
        if (verbose)
            std::printf("    %-38s           %-8s log %.2f exp %+.2f %5.0fK %+5.1f con %.1f sat %.3f | out exp %+.2f %5.0fK %+5.1f con %.1f\n",
                        "(untouched)", transferShortName(base.source.transfer), base.source.logScore, base.source.exposure,
                        base.source.cct, base.source.tint, base.source.contrast, base.source.saturation, base.result.exposure,
                        base.result.cct, base.result.tint, base.result.contrast);
        std::printf("  %-50s %s exp %+.2f %5.0fK tint %+5.1f contrast %.1f | ref %+.2f %5.0fK\n", fname.c_str(),
                    transferShortName(base.source.transfer), base.source.exposure, base.source.cct, base.source.tint,
                    base.source.contrast, base.reference.exposure, base.reference.cct);
        std::fflush(stdout);
    }

    std::printf("\n%-14s %-38s %8s %8s %8s %s\n", "group", "perturbation", "mean", "p95", "worst", runOld ? "   old mean / p95" : "");
    std::map<std::string, std::vector<double>> groupMean, groupP95;
    for (auto& p : perts) {
        Acc& a = byName[p.name];
        double mean = 0, p95 = 0, worst = 0;
        int n = 0, clippedN = 0;
        for (size_t i = 0; i < a.means.size(); ++i) {
            if (a.clipped[i]) { ++clippedN; continue; }
            mean += a.means[i]; p95 += a.p95s[i]; worst = std::max(worst, a.p95s[i]); ++n;
        }
        double cm = 0, cp = 0;
        for (size_t i = 0; i < a.means.size(); ++i) if (a.clipped[i]) { cm += a.means[i]; cp += a.p95s[i]; }
        char old[64] = "";
        if (runOld) {
            Acc& o = byNameOld[p.name];
            double om = 0, op = 0;
            for (size_t i = 0; i < o.means.size(); ++i) { om += o.means[i]; op += o.p95s[i]; }
            std::snprintf(old, sizeof old, "   %6.2f / %6.2f", om / o.means.size(), op / o.p95s.size());
        }
        if (n) {
            mean /= n; p95 /= n;
            std::printf("%-14s %-38s %8.2f %8.2f %8.2f%s%s\n", p.group.c_str(), p.name.c_str(), mean, p95, worst, old,
                        clippedN ? (" (+" + std::to_string(clippedN) + " clipped)").c_str() : "");
            groupMean[p.group].push_back(mean);
            groupP95[p.group].push_back(p95);
            CHECK(mean <= 2.0 && p95 <= 5.0, "%s: mean %.2f p95 %.2f (target <= 2 / <= 5)", p.name.c_str(), mean, p95);
        }
        if (clippedN) {
            cm /= clippedN; cp /= clippedN;
            std::printf("%-14s %-38s %8.2f %8.2f          (clipped: %d frame(s))%s\n", p.group.c_str(), (p.name + " [clipped]").c_str(), cm,
                        cp, clippedN, n ? "" : old);
            CHECK(cm <= 5.0 && cp <= 12.0, "%s (clipped): mean %.2f p95 %.2f (target <= 5 / <= 12)", p.name.c_str(), cm, cp);
        }
    }
    std::printf("\ncolour relationships kept in %d of %d frames (hue sectors keep their order and colour)\n",
                contentChecked - contentFails, contentChecked);
    std::printf("log detected in %d of %d log-encoded frames; display frames taken for log: %d\n", detectedLog, logCases, falseLog);
    std::printf("skin: %d frame(s) with skin, %d flagged (ashy / orange / split); mean skin ΔE00 vs the untouched match:", skinFrames, skinFlags);
    for (auto& [g, v] : skinByGroup) {
        double m = 0;
        for (double x : v) m += x;
        std::printf(" %s %.2f", g.c_str(), v.empty() ? 0.0 : m / v.size());
    }
    std::printf("\n");
    std::printf("largest step along the grey axis: %.2f/255 per 1/1024; largest LUT / exact local gain on smooth gradients: %.2f\n",
                worstJump, worstGain);
    CHECK(falseLog == 0, "display-referred frames were taken for log %d time(s)", falseLog);

    if (!sheet.empty() && !sheetTiles.empty()) {  // contact sheet: pairs of input / matched, 4 pairs a row
        const int tw = sheetTiles[0].first.width, th = sheetTiles[0].first.height, cols = 8;
        const int rows = int((sheetTiles.size() + cols - 1) / cols);
        Image s;
        s.width = cols * tw; s.height = rows * th;
        s.px.assign(size_t(s.width) * s.height, Vec3(0.1f, 0.1f, 0.1f));
        for (size_t k = 0; k < sheetTiles.size(); ++k) {
            const Image& t = sheetTiles[k].first;
            int ox = int(k % cols) * tw, oy = int(k / cols) * th;
            for (int y = 0; y < std::min(th, t.height); ++y)
                for (int x = 0; x < std::min(tw, t.width); ++x) s.px[size_t(oy + y) * s.width + ox + x] = t.at(x, y);
        }
        savePng(sheet, s);
        std::printf("contact sheet: %s\n", sheet.c_str());
    }
    std::printf(failures ? "%d check(s) failed\n" : "all consistency checks passed\n", failures);
    return failures ? 1 : 0;
}
