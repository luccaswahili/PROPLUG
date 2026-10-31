#include "engine.h"
#include <cstdint>
#include <cstring>
#include "parallel.h"

namespace lm {
namespace {

// ---------- noise ----------

inline uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline uint32_t hash4(int x, int y, long f, int c) {
    return hash(uint32_t(x) * 0x9E3779B1U ^ hash(uint32_t(y) * 0x85EBCA77U ^ hash(uint32_t(f) * 0xC2B2AE3DU ^ uint32_t(c) * 0x27D4EB2FU)));
}
// roughly unit-variance, zero-mean (triangular distribution)
inline float noise(int x, int y, long f, int c) {
    uint32_t h = hash4(x, y, f, c);
    return ((h & 0xFFFF) + (h >> 16)) * (1.0f / 65535.0f) * 2.449f - 2.449f;
}
inline float smooth01(float t) { return t * t * (3 - 2 * t); }
inline float smoothstep(float a, float b, float v) { return smooth01(std::clamp((v - a) / (b - a), 0.0f, 1.0f)); }
// smooth 1D noise over time, -1..1
float timeNoise(float t, int seed) {
    int i = int(std::floor(t));
    float f = smooth01(t - i);
    auto v = [&](int k) { return (hash(uint32_t(k) * 747796405U + uint32_t(seed) * 2891336453U) & 0xFFFF) / 32767.5f - 1.0f; };
    return v(i) + (v(i + 1) - v(i)) * f;
}

// ---------- blur ----------

// separable box blur, radius r, applied `passes` times (3 passes ~ gaussian). buf is w*h*ch.
void boxBlur(std::vector<float>& buf, std::vector<float>& tmp, int w, int h, int ch, int r, int passes = 3) {
    if (r < 1) return;
    tmp.resize(buf.size());
    const float inv = 1.0f / float(2 * r + 1);
    for (int p = 0; p < passes; ++p) {
        parallelFor(0, h, [&](int y) {  // horizontal: buf -> tmp
            const float* src = &buf[size_t(y) * w * ch];
            float* dst = &tmp[size_t(y) * w * ch];
            for (int c = 0; c < ch; ++c) {
                float acc = 0;
                for (int k = -r; k <= r; ++k) acc += src[std::clamp(k, 0, w - 1) * ch + c];
                for (int x = 0; x < w; ++x) {
                    dst[x * ch + c] = acc * inv;
                    acc += src[std::min(x + r + 1, w - 1) * ch + c] - src[std::max(x - r, 0) * ch + c];
                }
            }
        });
        parallelFor(0, w, [&](int x) {  // vertical: tmp -> buf
            for (int c = 0; c < ch; ++c) {
                float acc = 0;
                for (int k = -r; k <= r; ++k) acc += tmp[(size_t(std::clamp(k, 0, h - 1)) * w + x) * ch + c];
                for (int y = 0; y < h; ++y) {
                    buf[(size_t(y) * w + x) * ch + c] = acc * inv;
                    acc += tmp[(size_t(std::min(y + r + 1, h - 1)) * w + x) * ch + c] - tmp[(size_t(std::max(y - r, 0)) * w + x) * ch + c];
                }
            }
        });
    }
}

// bilinear sample of a w*h*3 buffer at continuous coords
inline Vec3 sample3(const std::vector<float>& b, int w, int h, float x, float y) {
    x = std::clamp(x, 0.0f, float(w - 1)); y = std::clamp(y, 0.0f, float(h - 1));
    int x0 = std::min(int(x), w - 2 < 0 ? 0 : w - 2), y0 = std::min(int(y), h - 2 < 0 ? 0 : h - 2);
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy) { const float* p = &b[(size_t(yy) * w + xx) * 3]; return Vec3(p[0], p[1], p[2]); };
    return lerp(lerp(at(x0, y0), at(x1, y0), fx), lerp(at(x0, y1), at(x1, y1), fx), fy);
}

// Glow: threshold the light, blur it at 1/4 resolution, return the blurred buffer (w4*h4*3).
void glowPass(const std::vector<float>& lin, int w, int h, float threshold, int radius, std::vector<float>& out,
              std::vector<float>& tmp, int& w4, int& h4) {
    const int f = 4;
    w4 = std::max(1, (w + f - 1) / f); h4 = std::max(1, (h + f - 1) / f);
    out.assign(size_t(w4) * h4 * 3, 0.0f);
    parallelFor(0, h4, [&](int y4) {
        for (int x4 = 0; x4 < w4; ++x4) {
            Vec3 acc; int n = 0;
            for (int yy = y4 * f; yy < std::min(h, y4 * f + f); ++yy)
                for (int xx = x4 * f; xx < std::min(w, x4 * f + f); ++xx) {
                    const float* p = &lin[(size_t(yy) * w + xx) * 3];
                    Vec3 c(p[0], p[1], p[2]);
                    float l = luma709(c);
                    float k = l > threshold ? (l - threshold) / std::max(l, 1e-6f) : 0.0f;  // keep hue, drop below threshold
                    acc = acc + c * k; ++n;
                }
            float* o = &out[(size_t(y4) * w4 + x4) * 3];
            o[0] = acc.r / n; o[1] = acc.g / n; o[2] = acc.b / n;
        }
    });
    boxBlur(out, tmp, w4, h4, 3, std::max(1, radius / f));
}

// LUT shaper: log2 with an offset, covers scene linear 0 .. ~64 with plenty of codes in the shadows.
// The span is chosen so linear 1.0 (Rec.709 white, where the neutral transform clips) sits exactly
// on a grid node at 5/8 of the range for 17, 33 and 65 point LUTs, so white doesn't get interpolated across the clip.
constexpr float kShaperOff = 1.0f / 1024.0f;
const float kShaperStops = (std::log2(1.0f + kShaperOff) + 10.0f) / 0.625f;
inline float shaperEncode(float x) { return clamp01((std::log2(std::max(x, 0.0f) + kShaperOff) + 10.0f) / kShaperStops); }
inline float shaperDecode(float v) { return std::max(0.0f, std::exp2(v * kShaperStops - 10.0f) - kShaperOff); }

inline float shoulder(float x) {  // slope-1 soft clip above 0.8, asymptote 1
    const float k = 0.8f;
    return x <= k ? x : k + (1 - k) * (1 - std::exp(-(x - k) / (1 - k)));
}

}  // namespace

Engine::Engine() { setParams(Params{}); }

void Engine::setParams(const Params& p) {
    const Params& o = p_;
    bool colour = lutDirty_ || p.input != o.input || p.matchStrength != o.matchStrength || p.filmStrength != o.filmStrength ||
                  p.film.negative != o.film.negative || p.film.print != o.film.print || p.film.saturation != o.film.saturation ||
                  std::memcmp(p.film.printerPoints, o.film.printerPoints, sizeof o.film.printerPoints) != 0 || p.fade != o.fade ||
                  p.adjust != o.adjust || p.lutSize != o.lutSize;
    bool gainChanged = p.exposure != o.exposure || p.temperature != o.temperature || p.tint != o.tint;
    p_ = p;
    if (colour) {
        film_ = std::make_unique<FilmModel>(p_.film);
        lutDirty_ = true;
    }
    if (colour || gainChanged) directDirty_ = true;
}

// Whether the colour chain changes the picture at all: with nothing to expand, the source's steps
// stay as invisible as they were, so deband is off and an untouched clip passes through exactly.
bool Engine::grades() const {
    return (match_ && p_.matchStrength > 0) || p_.filmStrength > 0 || p_.fade > 0 || p_.exposure != 0 ||
           p_.temperature != 0 || p_.tint != 0 || p_.input != InputSpace::Rec709 || p_.adjust.active();
}

void Engine::setMatchLut(std::shared_ptr<const Lut3D> lut) {
    match_ = std::move(lut);
    lutDirty_ = true;
    directDirty_ = true;
}

Vec3 Engine::inputGain() const {
    float e = std::exp2(p_.exposure);
    // warm = more red / less blue, magenta = less green; normalised to keep luminance
    Vec3 g(std::exp2(0.3f * p_.temperature), std::exp2(-0.2f * p_.tint), std::exp2(-0.3f * p_.temperature));
    return g * (e / luma709(g));
}

// neutral display: identity for Rec.709 footage, soft highlight roll-off for log footage
static Vec3 displayNeutral(InputSpace s, Vec3 lin) {
    if (s == InputSpace::Rec709) return {rec709Encode(std::min(lin.r, 1.0f)), rec709Encode(std::min(lin.g, 1.0f)), rec709Encode(std::min(lin.b, 1.0f))};
    return {rec709Encode(shoulder(lin.r)), rec709Encode(shoulder(lin.g)), rec709Encode(shoulder(lin.b))};
}

Vec3 Engine::colorFromLinear(Vec3 lin) const {
    lin = {std::max(lin.r, 0.0f), std::max(lin.g, 0.0f), std::max(lin.b, 0.0f)};
    Vec3 disp = displayNeutral(p_.input, lin);

    Vec3 out = disp;
    if (match_ && p_.matchStrength > 0) out = lerp(disp, match_->apply(disp), p_.matchStrength);

    if (film_ && p_.filmStrength > 0) {
        // carry the match into scene light as a per-channel gain, so film still sees log latitude
        const float eps = 0.002f;
        Vec3 filmIn;
        for (int c = 0; c < 3; ++c) filmIn[c] = lin[c] * (rec709Decode(out[c]) + eps) / (rec709Decode(disp[c]) + eps);
        out = lerp(out, film_->apply(filmIn), p_.filmStrength);
    }
    if (p_.adjust.active()) out = applyAdjust(clamp01(out), p_.adjust);
    if (p_.fade > 0) {
        float lift = 0.25f * p_.fade;
        out = Vec3(lift, lift, lift) + out * (1 - lift);
    }
    return clamp01(out);
}

void Engine::rebuildLut() {
    lut_ = Lut3D(p_.lutSize);
    lut_.fill([&](Vec3 code) { return colorFromLinear({shaperDecode(code.r), shaperDecode(code.g), shaperDecode(code.b)}); });
    lutDirty_ = false;
}

Vec3 Engine::neutralDisplay(Vec3 code) const {
    Vec3 lin = gamutToRec709(p_.input) * decode(p_.input, code) * inputGain();
    return clamp01(displayNeutral(p_.input, {std::max(lin.r, 0.0f), std::max(lin.g, 0.0f), std::max(lin.b, 0.0f)}));
}

Lut3D Engine::bakeExportLut(int size) const { return bakeLut(size, false); }

Lut3D Engine::bakeLut(int size, bool shaped) const {
    Lut3D l(size);
    // the direct (GPU / fast path) LUT samples the match where its own grid is dense: same shaper
    if (shaped && match_ && !match_->shaper().empty()) l.setShaper(match_->shaper());
    const Mat3& M = gamutToRec709(p_.input);
    Vec3 g = inputGain();
    // Same range limit as render's shaped LUT: scene linear is clamped to the shaper's range
    // (log footage near clip decodes far above it), so the export and the fast path match a render.
    auto limit = [](float v) { return shaperDecode(clamp01(shaperEncode(v))); };
    l.fill([&](Vec3 code) {
        Vec3 lin = M * decode(p_.input, code) * g;
        return colorFromLinear({limit(lin.r), limit(lin.g), limit(lin.b)});
    });
    return l;
}

DebandParams debandParams(float amount, int height) {
    DebandParams d;
    if (amount <= 0) return d;
    // 40 px at 1080p: about as wide as a compressed sky's flat steps
    d.radius = std::max(1, int(std::lround(40.0f * height / 1080.0f)));
    // ~3 8-bit or ~12 10-bit codes: a compressed sky's steps and its gradient across the radius stay
    // below it; edges and most texture are above it (tests/banding_tests.cpp checks both)
    d.threshold = 0.012f * std::min(1.0f, amount);
    d.dither = 0.0008f * std::min(1.0f, amount);
    d.nearRadius = std::max(1, int(std::lround(3.0f * height / 1080.0f)));
    d.coring = 0.0015f;
    return d;
}

// Keep in step with deband() in LookMatchKernel.h: same hash, same sample order.
Vec3 debandAt(const float* data, int width, int height, int stride, int x, int y, const DebandParams& d) {
    const float* p = data + size_t(y) * stride + size_t(x) * 4;
    const Vec3 c(p[0], p[1], p[2]);
    if (d.radius <= 0) return c;
    const uint32_t span = uint32_t(2 * d.radius + 1), nearSpan = uint32_t(2 * d.nearRadius + 1);
    // Point-symmetric pairs, each counted by its worse side: on a gradient the two sides cancel, so which
    // pairs were picked doesn't show. Counting sides one by one left a different lopsided average on every
    // pixel of shaded skin: per-pixel colour speckle. The near ring still passes on shaded skin, where the
    // far pairs are mostly refused, and takes out the source's fine chroma noise.
    Vec3 sum = c;
    float wsum = 1;
    for (int k = 0; k < kDebandPairs + kDebandNearPairs; ++k) {
        const uint32_t h = hash4(x, y, 0x5EED, k);
        const uint32_t sp = k < kDebandPairs ? span : nearSpan;
        const int rad = k < kDebandPairs ? d.radius : d.nearRadius;
        const int dx = int((h & 0xFFFFu) % sp) - rad, dy = int((h >> 16) % sp) - rad;
        const int xa = std::clamp(x + dx, 0, width - 1), ya = std::clamp(y + dy, 0, height - 1);
        const int xb = std::clamp(x - dx, 0, width - 1), yb = std::clamp(y - dy, 0, height - 1);
        const float* qa = data + size_t(ya) * stride + size_t(xa) * 4;
        const float* qb = data + size_t(yb) * stride + size_t(xb) * 4;
        // how far the pair's midpoint is from the pixel (zero on a straight gradient), and each side
        // within twice the threshold, so a pair across an edge doesn't count
        const float mid = std::max({std::abs(0.5f * (qa[0] + qb[0]) - c.r), std::abs(0.5f * (qa[1] + qb[1]) - c.g),
                                    std::abs(0.5f * (qa[2] + qb[2]) - c.b)});
        const float side = 0.5f * std::max({std::abs(qa[0] - c.r), std::abs(qa[1] - c.g), std::abs(qa[2] - c.b),
                                            std::abs(qb[0] - c.r), std::abs(qb[1] - c.g), std::abs(qb[2] - c.b)});
        // the near ring only takes very small differences: noise, not freckles or fine colour detail
        const float t = k < kDebandPairs ? d.threshold : 0.5f * d.threshold;
        const float w = 1 - smoothstep(0.5f * t, t, std::max(mid, side));
        sum = sum + (Vec3(qa[0], qa[1], qa[2]) + Vec3(qb[0], qb[1], qb[2])) * w;
        wsum += 2 * w;
    }
    Vec3 o = sum * (1.0f / wsum);
    // fine texture stays: the pixel's brightness relative to the average comes back, cored, so deviations
    // of the size of grain and of a compressed sky's steps stay out and real texture stays in
    const float dev = ((c.r + c.g + c.b) - (o.r + o.g + o.b)) / 3;
    const float back = dev > d.coring ? dev - d.coring : dev < -d.coring ? dev + d.coring : 0.0f;
    // dither where it smoothed: the same on all channels, so it adds no colour noise
    const float amount = (wsum - 1) / (2 * (kDebandPairs + kDebandNearPairs));
    const uint32_t h = hash4(x, y, 0x5EED, 64);
    const float dith = (float((h & 0xFFFFu) + (h >> 16)) * (1.0f / 65535.0f) - 1.0f) * d.dither * amount;
    return {o.r + back + dith, o.g + back + dith, o.b + back + dith};
}

SkinEvenParams skinEvenParams(float amount, int height) {
    SkinEvenParams s;
    if (amount > 0) { s.radius = std::max(2, int(240.0f * height / 4320.0f + 0.5f)); s.amount = amount; }  // 240 px at 8K, 60 at 1080p
    return s;
}

namespace {
const float kSkinDisc[kSkinSamples][2] = {
    {0.1443f, 0.0000f}, {-0.1843f, 0.1689f}, {0.0282f, -0.3215f}, {0.2324f, 0.3031f},
    {-0.4264f, -0.0754f}, {0.4039f, -0.2569f}, {-0.1351f, 0.5026f}, {-0.2577f, -0.4961f},
    {0.5590f, 0.2041f}, {-0.5816f, 0.2401f}, {0.2803f, -0.5991f}, {0.2072f, 0.6605f},
    {-0.6244f, -0.3619f}, {0.7325f, -0.1610f}, {-0.4470f, 0.6359f}, {-0.1033f, -0.7970f},
    {0.6340f, 0.5343f}, {-0.8532f, 0.0353f}, {0.6223f, -0.6193f}, {-0.0416f, 0.9004f},
    {-0.5922f, -0.7096f}, {0.9380f, 0.1262f}, {-0.7948f, 0.5530f}, {0.2172f, -0.9654f},
};
float step01(float a, float b, float v) { const float t = clamp01((v - a) / (b - a)); return t * t * (3 - 2 * t); }
// how much a code value looks like skin: warm, between the red of lips and the yellow of wood, a little colour
float skinMask(Vec3 c) {
    const float d1 = c.r - c.g, d2 = c.g - c.b, sat = c.r - c.b;
    if (d1 <= 0 || d2 <= 0) return 0;
    const float hue = d2 / (d1 + d2), y = (c.r + c.g + c.b) / 3;
    return step01(0.22f, 0.29f, hue) * (1 - step01(0.46f, 0.52f, hue)) * step01(0.012f, 0.03f, sat) * (1 - step01(0.16f, 0.22f, sat)) *
           step01(0.10f, 0.18f, y) * (1 - step01(0.90f, 0.97f, y));
}
}  // namespace

// Keep in step with skinEven() in LookMatchKernel.h: same mask, same samples.
Vec3 skinEvenAt(const float* data, int width, int height, int stride, int x, int y, Vec3 base, const SkinEvenParams& s) {
    if (s.radius <= 0) return {0, 0, 0};
    auto at = [&](int xx, int yy) {
        const float* p = data + size_t(yy) * stride + size_t(xx) * 4;
        return Vec3(p[0], p[1], p[2]);
    };
    const Vec3 c = at(x, y);
    const float m = skinMask(c);
    if (m <= 0) return {0, 0, 0};
    const float yc = (c.r + c.g + c.b) / 3;
    const Vec3 vc(c.r - yc, c.g - yc, c.b - yc);
    Vec3 sum = vc;
    float wsum = 1;
    for (int k = 0; k < kSkinSamples; ++k) {
        const int xx = std::clamp(x + int(std::floor(kSkinDisc[k][0] * s.radius + 0.5f)), 0, width - 1);
        const int yy = std::clamp(y + int(std::floor(kSkinDisc[k][1] * s.radius + 0.5f)), 0, height - 1);
        const Vec3 q = at(xx, yy);
        const float yq = (q.r + q.g + q.b) / 3;
        const Vec3 vq(q.r - yq, q.g - yq, q.b - yq);
        const float dc = std::max({std::abs(vq.r - vc.r), std::abs(vq.g - vc.g), std::abs(vq.b - vc.b)});
        const float w = skinMask(q) * (1 - step01(0.03f, 0.08f, std::abs(yq - yc))) * (1 - step01(0.012f, 0.03f, dc));
        sum = sum + vq * w;
        wsum += w;
    }
    const float yb = (base.r + base.g + base.b) / 3, t = 1 / wsum;
    return Vec3(sum.r * t - (base.r - yb), sum.g * t - (base.g - yb), sum.b * t - (base.b - yb)) * (m * s.amount);
}

void Engine::render(const ImageView& in, const ImageView& out, long frame) {
    const DebandParams db = debandParams(grades() ? p_.deband : 0, in.height);
    const SkinEvenParams ss = skinEvenParams(match_ && p_.matchStrength > 0 ? p_.skinEven : 0, in.height);
    auto source = [&](int x, int y) {
        const Vec3 c = debandAt(in.data, in.width, in.height, in.stride, x, y, db);
        return c + skinEvenAt(in.data, in.width, in.height, in.stride, x, y, c, ss);
    };
    const int w = in.width, h = in.height;
    // Fast path: with no weave, flicker or light spreading, the whole colour chain (decode, gain,
    // match, film, fade) is one function of the input pixel, baked into a single 3D LUT.
    if (p_.halation <= 0 && p_.bloom <= 0 && p_.softness <= 0 && p_.weave <= 0 && p_.flicker <= 0) {
        if (p_.input == InputSpace::Rec709) {
            if (directDirty_) {
                direct_ = bakeLut(p_.lutSize, true);
                directDirty_ = false;
            }
            finish(in, out, frame, [&](int x, int y) { return direct_.apply(source(x, y)); });
        } else {
            // Log footage: near clip, saturated highlights make the transform jump (gamut matrix on
            // clipped channels), which one baked LUT would smooth over. Decode exactly per pixel instead,
            // fused into the final pass (no scene-linear buffer).
            if (lutDirty_) rebuildLut();
            const Mat3& M = gamutToRec709(p_.input);
            const Vec3 gain = inputGain();
            finish(in, out, frame, [&](int x, int y) {
                Vec3 l = M * decode(p_.input, source(x, y)) * gain;
                return lut_.apply({shaperEncode(l.r), shaperEncode(l.g), shaperEncode(l.b)});
            });
        }
        return;
    }
    if (lutDirty_) rebuildLut();
    const float scale = h / 1080.0f;  // spatial settings are authored at 1080p
    const NegativeStock& stock = negativeStock(p_.film.negative);
    const float filmMix = p_.filmStrength;
    // film stocks nudge halation (B&W has no red halation, 800T has lots); grain is nudged in finish()
    const float stockHal = 1 + filmMix * (stock.halation / 0.4f - 1);

    // 1+2. weave + decode to scene linear
    const Mat3& M = gamutToRec709(p_.input);
    Vec3 gain = inputGain();
    if (p_.flicker > 0) gain = gain * std::exp2(p_.flicker * 0.12f * timeNoise(frame * 0.9f, 7));
    float dx = 0, dy = 0;
    if (p_.weave > 0) {
        dx = p_.weave * scale * timeNoise(frame * 0.35f, 1);
        dy = p_.weave * scale * 0.6f * timeNoise(frame * 0.35f, 2);
    }
    lin_.resize(size_t(w) * h * 3);
    parallelFor(0, h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            Vec3 c;
            if (dx != 0 || dy != 0) {
                float sx = std::clamp(x + dx, 0.0f, float(w - 1)), sy = std::clamp(y + dy, 0.0f, float(h - 1));
                int x0 = std::min(int(sx), std::max(0, w - 2)), y0 = std::min(int(sy), std::max(0, h - 2));
                int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                float fx = sx - x0, fy = sy - y0;
                auto at = [&](int xx, int yy) { const float* p = in.px(xx, yy); return Vec3(p[0], p[1], p[2]); };
                c = lerp(lerp(at(x0, y0), at(x1, y0), fx), lerp(at(x0, y1), at(x1, y1), fx), fy);
            } else {
                c = source(x, y);
            }
            Vec3 l = M * decode(p_.input, c) * gain;
            float* o = &lin_[(size_t(y) * w + x) * 3];
            o[0] = l.r; o[1] = l.g; o[2] = l.b;
        }
    });

    // 3. halation, bloom, softness (scene linear)
    int w4, h4;
    if (p_.halation * stockHal > 0) {
        glowPass(lin_, w, h, p_.halationThreshold, int(18 * p_.halationRadius * scale), tmpA_, tmpB_, w4, h4);
        const float s = p_.halation * stockHal * 1.5f;
        const Vec3 tint(1.0f, 0.28f, 0.06f);  // red layer scatter off the film base
        parallelFor(0, h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                Vec3 g = sample3(tmpA_, w4, h4, (x + 0.5f) / 4 - 0.5f, (y + 0.5f) / 4 - 0.5f);
                float* o = &lin_[(size_t(y) * w + x) * 3];
                float v = luma709(g) * s;
                o[0] += v * tint.r; o[1] += v * tint.g; o[2] += v * tint.b;
            }
        });
    }
    if (p_.bloom > 0) {
        glowPass(lin_, w, h, p_.bloomThreshold, int(60 * p_.bloomRadius * scale), tmpA_, tmpB_, w4, h4);
        const float s = p_.bloom;
        parallelFor(0, h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                Vec3 g = sample3(tmpA_, w4, h4, (x + 0.5f) / 4 - 0.5f, (y + 0.5f) / 4 - 0.5f);
                float* o = &lin_[(size_t(y) * w + x) * 3];
                o[0] += g.r * s; o[1] += g.g * s; o[2] += g.b * s;
            }
        });
    }
    if (p_.softness > 0) {
        tmpA_ = lin_;
        boxBlur(tmpA_, tmpB_, w, h, 3, std::max(1, int(std::round(1.5f * scale))), 2);
        const float s = std::min(1.0f, p_.softness);
        for (size_t i = 0; i < lin_.size(); ++i) lin_[i] += (tmpA_[i] - lin_[i]) * s;
    }

    // 4+5. colour LUT, grain, vignette, mix
    finish(in, out, frame, [&](int x, int y) {
        const float* l = &lin_[(size_t(y) * w + x) * 3];
        return lut_.apply({shaperEncode(l[0]), shaperEncode(l[1]), shaperEncode(l[2])});
    });
}

bool Engine::prepareDirect(int w, int h, long frame, FinishParams& fp) {
    if (p_.halation > 0 || p_.bloom > 0 || p_.softness > 0 || p_.weave > 0 || p_.flicker > 0 ||
        p_.input != InputSpace::Rec709)
        return false;
    if (directDirty_) {
        direct_ = bakeLut(p_.lutSize, true);
        directDirty_ = false;
    }
    const float scale = h / 1080.0f;
    const NegativeStock& stock = negativeStock(p_.film.negative);
    const float filmMix = p_.filmStrength;
    fp.grainAmp = p_.grain * (1 + filmMix * (stock.grain / 0.4f - 1)) * 0.09f;
    fp.cell = std::max(0.5f, p_.grainSize * (1 + filmMix * (stock.grainSize - 1)) * scale);
    fp.grainColor = p_.grainColor;
    fp.vignette = p_.vignette;
    fp.mix = p_.mix;
    fp.cx = w * 0.5f;
    fp.cy = h * 0.5f;
    fp.invDiag = 1.0f / std::sqrt(fp.cx * fp.cx + fp.cy * fp.cy);
    fp.frame = int(frame);
    fp.deband = debandParams(grades() ? p_.deband : 0, h);
    fp.skin = skinEvenParams(match_ && p_.matchStrength > 0 ? p_.skinEven : 0, h);
    return true;
}

// Grain, vignette and mix on top of the graded colour `colourAt(x, y)`, written to `out`.
template <class ColourAt>
void Engine::finish(const ImageView& in, const ImageView& out, long frame, ColourAt colourAt) {
    const int w = in.width, h = in.height;
    const float scale = h / 1080.0f;
    const NegativeStock& stock = negativeStock(p_.film.negative);
    const float filmMix = p_.filmStrength;
    const float stockGrain = 1 + filmMix * (stock.grain / 0.4f - 1);
    const float stockGrainSize = 1 + filmMix * (stock.grainSize - 1);
    const float grainAmp = p_.grain * stockGrain * 0.09f;
    const float cell = std::max(0.5f, p_.grainSize * stockGrainSize * scale);
    const float cx = w * 0.5f, cy = h * 0.5f, invDiag = 1.0f / std::sqrt(cx * cx + cy * cy);
    parallelFor(0, h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            Vec3 c = colourAt(x, y);
            if (grainAmp > 0) {
                // value noise on a grid of `cell` pixels, smooth interpolated
                float gx = x / cell, gy = y / cell;
                int ix = int(std::floor(gx)), iy = int(std::floor(gy));
                float fx = smooth01(gx - ix), fy = smooth01(gy - iy);
                auto vn = [&](int ch) {
                    float a = noise(ix, iy, frame, ch), b = noise(ix + 1, iy, frame, ch);
                    float cc = noise(ix, iy + 1, frame, ch), d = noise(ix + 1, iy + 1, frame, ch);
                    return (a + (b - a) * fx) + ((cc + (d - cc) * fx) - (a + (b - a) * fx)) * fy;
                };
                float mono = vn(3);
                float y709 = luma709(c);
                float env = 0.15f + 1.7f * std::sqrt(std::max(0.0f, y709 * (1 - y709)));  // grain lives in the mids
                for (int ch = 0; ch < 3; ++ch) {
                    float n = mono + p_.grainColor * (vn(ch) - mono);
                    c[ch] += n * grainAmp * env;
                }
            }
            if (p_.vignette > 0) {
                float r = std::hypot(x - cx, y - cy) * invDiag;
                float v = smooth01(clamp01((r - 0.35f) / 0.65f));
                c = c * (1 - p_.vignette * 0.8f * v);
            }
            const float* src = in.px(x, y);
            float* o = out.px(x, y);
            float a = src[3];
            o[0] = src[0] + (clamp01(c.r) - src[0]) * p_.mix;
            o[1] = src[1] + (clamp01(c.g) - src[1]) * p_.mix;
            o[2] = src[2] + (clamp01(c.b) - src[2]) * p_.mix;
            o[3] = a;
        }
    });
}

}  // namespace lm
