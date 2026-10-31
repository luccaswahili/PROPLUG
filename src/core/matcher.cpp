#include "matcher.h"
#include <array>
#include <cstdio>
#include <numeric>

namespace lm {
namespace {

constexpr int kRegions = 8;  // neutral + 7 hue regions
constexpr float kNeutralC = 0.025f;
constexpr float kMinShare = 0.03f;
constexpr int kSkin = 2;
// Limits on how steep the match may get. Full histogram matching can stretch a narrow band
// (e.g. a frame that is mostly deep shadow) by 10x, turning compression blocks and 8-bit steps
// into visible posterisation. Colourists' tools cap this; so do we.
constexpr float kMaxToneSlope = 2.0f, kMinToneSlope = 0.3f;
constexpr double kMaxChromaGain = 2.0, kMinChromaGain = 0.5;
const char* kRegionNames[kRegions] = {"neutral", "red", "skin", "yellow", "green", "cyan", "blue", "magenta"};
// hue centre / sigma in degrees for regions 1..7. OKLab hue of human skin sits at ~40-68 deg.
const float kHueC[7] = {15, 55, 95, 140, 200, 260, 320};
const float kHueS[7] = {20, 16, 20, 25, 25, 25, 25};
const float kBands[3] = {0.2f, 0.5f, 0.8f};

using RW = std::array<float, kRegions>;

RW regionWeights(const Vec3& lab) {
    RW w{};
    float C = std::hypot(lab.g, lab.b);
    float h = std::atan2(lab.b, lab.g) * 57.29578f;
    float neutral = std::exp(-(C / kNeutralC) * (C / kNeutralC));
    float sum = 0;
    for (int i = 0; i < 7; ++i) {
        float d = std::fmod(h - kHueC[i] + 540.0f, 360.0f) - 180.0f;
        w[i + 1] = std::exp(-(d * d) / (2 * kHueS[i] * kHueS[i]));
        sum += w[i + 1];
    }
    for (int i = 1; i < kRegions; ++i) w[i] = w[i] / (sum + 1e-12f) * (1 - neutral);
    w[0] = neutral;
    return w;
}

float skinWeight(const Vec3& lab) {
    float C = std::hypot(lab.g, lab.b), L = lab.r;
    float gc = clamp01((C - 0.004f) / 0.02f) * clamp01((0.20f - C) / 0.05f);  // pale skin highlights count
    float gl = clamp01((L - 0.2f) / 0.1f) * clamp01((0.995f - L) / 0.03f);
    return regionWeights(lab)[kSkin] * gc * gl;
}

std::array<float, 3> bandWeights(float L) {
    std::array<float, 3> w;
    float s = 0;
    for (int i = 0; i < 3; ++i) { w[i] = std::exp(-(L - kBands[i]) * (L - kBands[i]) / (2 * 0.15f * 0.15f)); s += w[i]; }
    for (float& x : w) x /= s;
    return w;
}

// symmetric 2x2 matrix [a b; b c]
struct S2 { double a, b, c; };
struct M2 { double m00, m01, m10, m11; };

// V f(L) V^T for a symmetric 2x2 matrix with eigen-decomposition V L V^T
template <class F>
M2 eigMap(S2 s, F f) {
    double tr = s.a + s.c, det = s.a * s.c - s.b * s.b;
    double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
    double l1 = std::max(1e-12, tr / 2 + disc), l2 = std::max(1e-12, tr / 2 - disc);
    // eigenvectors
    double vx, vy;
    if (std::abs(s.b) > 1e-15) { vx = l1 - s.c; vy = s.b; } else if (s.a >= s.c) { vx = 1; vy = 0; } else { vx = 0; vy = 1; }
    double n = std::hypot(vx, vy); vx /= n; vy /= n;
    double r1 = f(l1), r2 = f(l2);
    // V diag(r) V^T with V = [[vx, -vy],[vy, vx]]
    return {r1 * vx * vx + r2 * vy * vy, (r1 - r2) * vx * vy, (r1 - r2) * vx * vy, r1 * vy * vy + r2 * vx * vx};
}
M2 sqrtPsd(S2 s) { return eigMap(s, [](double l) { return std::sqrt(l); }); }
M2 mul(M2 a, M2 b) {
    return {a.m00 * b.m00 + a.m01 * b.m10, a.m00 * b.m01 + a.m01 * b.m11, a.m10 * b.m00 + a.m11 * b.m10, a.m10 * b.m01 + a.m11 * b.m11};
}
M2 inv(M2 a) {
    double d = a.m00 * a.m11 - a.m01 * a.m10;
    return {a.m11 / d, -a.m01 / d, -a.m10 / d, a.m00 / d};
}

struct Stats {
    std::vector<Vec3> lab;
    std::vector<RW> rw;
    RW share{};
    std::vector<float> w;  // balanced weights
};

Stats analyse(const std::vector<Vec3>& rgb, float power) {
    Stats s;
    s.lab.resize(rgb.size());
    s.rw.resize(rgb.size());
    for (size_t i = 0; i < rgb.size(); ++i) {
        s.lab[i] = rgbToOklab(clamp01(rgb[i]));
        s.rw[i] = regionWeights(s.lab[i]);
        for (int r = 0; r < kRegions; ++r) s.share[r] += s.rw[i][r];
    }
    for (float& x : s.share) x /= float(rgb.size());
    RW inv{};
    for (int r = 0; r < kRegions; ++r) inv[r] = 1.0f / std::pow(std::max(s.share[r], 0.02f), power);
    s.w.resize(rgb.size());
    double mean = 0;
    for (size_t i = 0; i < rgb.size(); ++i) {
        float v = 0;
        for (int r = 0; r < kRegions; ++r) v += s.rw[i][r] * inv[r];
        s.w[i] = v;
        mean += v;
    }
    mean /= double(rgb.size());
    for (float& v : s.w) v = float(v / mean);
    return s;
}

std::vector<float> weightedQuantiles(const std::vector<Vec3>& lab, const std::vector<float>& w, int n) {
    std::vector<size_t> o(lab.size());
    std::iota(o.begin(), o.end(), 0);
    std::sort(o.begin(), o.end(), [&](size_t a, size_t b) { return lab[a].r < lab[b].r; });
    std::vector<double> pos(o.size());
    double cw = 0;
    for (size_t i = 0; i < o.size(); ++i) { cw += w[o[i]]; pos[i] = cw - 0.5 * w[o[i]]; }
    for (double& p : pos) p /= cw;
    std::vector<float> q(n);
    size_t j = 0;
    for (int k = 0; k < n; ++k) {
        double t = double(k) / (n - 1);
        while (j + 1 < pos.size() && pos[j + 1] < t) ++j;
        if (t <= pos.front()) q[k] = lab[o.front()].r;
        else if (t >= pos.back()) q[k] = lab[o.back()].r;
        else {
            double f = (t - pos[j]) / std::max(1e-12, pos[j + 1] - pos[j]);
            q[k] = float(lab[o[j]].r + f * (lab[o[j + 1]].r - lab[o[j]].r));
        }
    }
    return q;
}

float interp(float x, const std::vector<float>& xs, const std::vector<float>& ys) {
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    size_t i = std::upper_bound(xs.begin(), xs.end(), x) - xs.begin() - 1;
    float f = (x - xs[i]) / (xs[i + 1] - xs[i]);
    return ys[i] + f * (ys[i + 1] - ys[i]);
}

class Model {
public:
    Model(const std::vector<Vec3>& refRgb, const std::vector<Vec3>& srcRgb, const MatchOptions& opt)
        : rs_(opt.regionStrength), hueFloor_(opt.hueFloor) {
        Stats R = analyse(refRgb, opt.balance), S = analyse(srcRgb, opt.balance);
        shareRef_ = R.share; shareSrc_ = S.share;
        skinProtect_ = opt.skinProtect >= 0 ? opt.skinProtect : (R.share[kSkin] >= kMinShare ? opt.autoSkinProtect : 0.8f);

        // compare like with like: global stats from regions both images contain, each counted equally
        bool shared[kRegions]; float covR = 0, covS = 0; bool any = false;
        for (int r = 0; r < kRegions; ++r) {
            shared[r] = std::min(R.share[r], S.share[r]) >= kMinShare;
            if (shared[r]) { covR += R.share[r]; covS += S.share[r]; any = true; }
        }
        if (any && std::min(covR, covS) >= 0.2f) {
            for (Stats* st : {&R, &S})
                for (size_t i = 0; i < st->lab.size(); ++i) {
                    float v = 1e-4f;
                    for (int r = 0; r < kRegions; ++r) if (shared[r]) v += st->rw[i][r] / st->share[r];
                    st->w[i] = v;
                }
        }

        // 1. tone curve
        if (!opt.matchTone) {
            knots_.resize(256); curve_.resize(256);
            for (int i = 0; i < 256; ++i) knots_[i] = curve_[i] = i / 255.0f;
        } else {
            std::vector<float> sq = weightedQuantiles(S.lab, S.w, 257), rq = weightedQuantiles(R.lab, R.w, 257);
            for (size_t i = 0; i < sq.size(); ++i) sq[i] += float(i) * 1e-6f;
            for (size_t i = 1; i < sq.size(); ++i) sq[i] = std::max(sq[i], sq[i - 1]);
            knots_.resize(256); curve_.resize(256);
            for (int i = 0; i < 256; ++i) { knots_[i] = i / 255.0f; curve_[i] = interp(knots_[i], sq, rq); }
            std::vector<float> sm(256);
            for (int i = 0; i < 256; ++i) {
                float s = 0;
                for (int k = -4; k <= 4; ++k) s += curve_[std::clamp(i + k, 0, 255)];
                sm[i] = s / 9;
            }
            for (int i = 1; i < 256; ++i) sm[i] = std::max(sm[i], sm[i - 1]);
            curve_ = limitSlope(sm, sq);
        }

        // 2. Monge-Kantorovich chroma transfer
        // centre = -1: the weighted mean; 0: neutral (0, 0); 1: the mean of the near-neutral pixels.
        // About the mean, a frame that is mostly skin has its mean taken out, which turns its greys
        // the opposite way (cyan). About neutral, the frame's colours keep their direction.
        auto wcov = [](const Stats& st, double& ma, double& mb, int centre) {
            double sw = 0, sn = 0; ma = mb = 0;
            if (centre == 1) {
                for (size_t i = 0; i < st.lab.size(); ++i) { sn += st.rw[i][0]; ma += st.rw[i][0] * st.lab[i].g; mb += st.rw[i][0] * st.lab[i].b; }
                if (sn > 1e-6) { ma /= sn; mb /= sn; } else ma = mb = 0;
            }
            for (size_t i = 0; i < st.lab.size(); ++i) {
                sw += st.w[i];
                if (centre < 0) { ma += st.w[i] * st.lab[i].g; mb += st.w[i] * st.lab[i].b; }
            }
            if (centre < 0) { ma /= sw; mb /= sw; }
            S2 c{0, 0, 0};
            for (size_t i = 0; i < st.lab.size(); ++i) {
                double da = st.lab[i].g - ma, db = st.lab[i].b - mb;
                c.a += st.w[i] * da * da; c.b += st.w[i] * da * db; c.c += st.w[i] * db * db;
            }
            c.a = c.a / sw + 1e-6; c.b /= sw; c.c = c.c / sw + 1e-6;
            return c;
        };
        S2 cs = wcov(S, msA_, msB_, opt.chromaAboutNeutral ? 0 : -1), cr = wcov(R, mrA_, mrB_, opt.chromaAboutNeutral ? 1 : -1);
        if (opt.chromaAboutNeutral) {  // the look's tint of whites, gently: highlights on skin take it too
            const double t = std::hypot(mrA_, mrB_), cap = 0.4 * opt.maxColourShift;
            if (t > cap) { mrA_ *= cap / t; mrB_ *= cap / t; }
        }
        M2 csh = sqrtPsd(cs), cshi = inv(csh);
        M2 mid = mul(mul(csh, M2{cr.a, cr.b, cr.b, cr.c}), csh);
        M2 midS = sqrtPsd({mid.m00, 0.5 * (mid.m01 + mid.m10), mid.m11});
        M2 T = mul(mul(cshi, midS), cshi);
        // cap how much chroma spread is scaled along each axis
        T_ = eigMap({T.m00, 0.5 * (T.m01 + T.m10), T.m11},
                    [&](double l) { return std::clamp(l, kMinChromaGain, std::min(kMaxChromaGain, double(opt.maxChromaGain))); });

        // 3. region offsets
        std::vector<Vec3> mapped(S.lab.size());
        for (size_t i = 0; i < S.lab.size(); ++i) mapped[i] = global(S.lab[i]);
        for (int r = 0; r < kRegions; ++r) {
            float conf = std::min(R.share[r], S.share[r]);
            if (conf < kMinShare) continue;
            Vec3 mr, ms; double wr = 0, ws = 0;
            for (size_t i = 0; i < R.lab.size(); ++i) { mr = mr + R.lab[i] * R.rw[i][r]; wr += R.rw[i][r]; }
            for (size_t i = 0; i < S.lab.size(); ++i) { ms = ms + mapped[i] * S.rw[i][r]; ws += S.rw[i][r]; }
            Vec3 d = mr * float(1 / wr) - ms * float(1 / ws);
            d.r *= 0.5f;
            regionOffs_[r] = d * std::min(1.0f, (conf - kMinShare) / kMinShare);
            const float ab = std::hypot(regionOffs_[r].g, regionOffs_[r].b), capR = r ? opt.maxColourShift : 0.4f * opt.maxColourShift;
            if (ab > capR) { regionOffs_[r].g *= capR / ab; regionOffs_[r].b *= capR / ab; }
        }
        for (size_t i = 0; i < mapped.size(); ++i)
            for (int r = 0; r < kRegions; ++r) mapped[i] = mapped[i] + regionOffs_[r] * (rs_ * S.rw[i][r]);

        // 4. split toning per tonal band
        for (int b = 0; b < 3; ++b) {
            double wm = 0, wr = 0, ma = 0, mb = 0, ra = 0, rb = 0;
            for (size_t i = 0; i < mapped.size(); ++i) {
                float w = bandWeights(mapped[i].r)[b] * S.w[i];
                wm += w; ma += w * mapped[i].g; mb += w * mapped[i].b;
            }
            for (size_t i = 0; i < R.lab.size(); ++i) {
                float w = bandWeights(R.lab[i].r)[b] * R.w[i];
                wr += w; ra += w * R.lab[i].g; rb += w * R.lab[i].b;
            }
            if (wm < 50 || wr < 50) continue;
            bandA_[b] = float(ra / wr - ma / wm);
            bandB_[b] = float(rb / wr - mb / wm);
            const float ab = std::hypot(bandA_[b], bandB_[b]), cap = 0.7f * opt.maxColourShift;
            if (ab > cap) { bandA_[b] *= cap / ab; bandB_[b] *= cap / ab; }
        }
    }

    // Re-integrates the tone curve with its slope clamped. The minimum slope only applies inside
    // the source's tonal range (1st-99th percentile); outside it the curve may stay flat.
    std::vector<float> limitSlope(const std::vector<float>& c, const std::vector<float>& srcQuantiles) const {
        const int n = int(c.size());
        const float lo = srcQuantiles[srcQuantiles.size() / 100], hi = srcQuantiles[srcQuantiles.size() * 99 / 100];
        std::vector<float> out(n);
        out[0] = c[0];
        for (int i = 1; i < n; ++i) {
            float step = (c[i] - c[i - 1]) * (n - 1);
            float minSlope = knots_[i] >= lo && knots_[i] <= hi ? kMinToneSlope : 0.0f;
            out[i] = out[i - 1] + std::clamp(step, minSlope, kMaxToneSlope) / (n - 1);
        }
        // Pin both ends of the source's range to where the unclamped curve put them, so clamping a
        // steep stretch near one end doesn't drag the other end (e.g. dull, grey highlights).
        // The correction is spread evenly over the range, and is itself capped.
        const float cLo = interp(lo, knots_, c), cHi = interp(hi, knots_, c);
        const float oLo = interp(lo, knots_, out), oHi = interp(hi, knots_, out);
        const float k = oHi - oLo > 1e-4f ? std::clamp((cHi - cLo) / (oHi - oLo), 0.5f, 2.0f) : 1.0f;
        for (float& v : out) v = clamp01(cLo + (v - oLo) * k);
        return out;
    }

    Vec3 global(const Vec3& lab) const {
        double a = lab.g - msA_, b = lab.b - msB_;
        return {interp(clamp01(lab.r), knots_, curve_), float(T_.m00 * a + T_.m01 * b + mrA_), float(T_.m10 * a + T_.m11 * b + mrB_)};
    }

    Vec3 applyLab(const Vec3& lab) const {
        Vec3 out = global(lab);
        RW w = regionWeights(lab);
        // Hue regions only where there is hue to speak of: a nearly grey wall's hue flips with every
        // 8-bit step of its chroma, and so would its region offset (rings across the wall).
        const float hueConf = hueFloor_ > 0 ? clamp01((std::hypot(lab.g, lab.b) - hueFloor_) / (2 * hueFloor_)) : 1.0f;
        for (int r = 0; r < kRegions; ++r) out = out + regionOffs_[r] * (rs_ * w[r] * (r ? hueConf : 1.0f));
        auto bw = bandWeights(out.r);
        for (int b = 0; b < 3; ++b) { out.g += bw[b] * bandA_[b]; out.b += bw[b] * bandB_[b]; }
        // skin protect: hold skin hue within 8 degrees and chroma within -20%/+25%
        float s = skinProtect_ * skinWeight(lab);
        if (s > 0) {
            float Ci = std::hypot(lab.g, lab.b), hi = std::atan2(lab.b, lab.g);
            float Co = std::hypot(out.g, out.b), ho = std::atan2(out.b, out.g);
            float dh = std::remainder(ho - hi, 6.2831853f);
            dh = std::clamp(dh, -0.1396263f, 0.1396263f);
            float Cp = std::clamp(Co, 0.8f * Ci, 1.25f * Ci), hp = hi + dh;
            out.g += s * (Cp * std::cos(hp) - out.g);
            out.b += s * (Cp * std::sin(hp) - out.b);
        }
        return out;
    }

    std::string report() const {
        std::string s;
        char buf[128];
        std::snprintf(buf, sizeof buf, "skin protect %.1f\nregion      ref%%   src%%   matched\n", skinProtect_);
        s += buf;
        for (int r = 0; r < kRegions; ++r) {
            bool m = regionOffs_[r].r != 0 || regionOffs_[r].g != 0 || regionOffs_[r].b != 0;
            std::snprintf(buf, sizeof buf, "%-10s%5.1f  %5.1f   %s\n", kRegionNames[r], 100 * shareRef_[r], 100 * shareSrc_[r], m ? "yes" : "-");
            s += buf;
        }
        return s;
    }

private:
    float rs_, skinProtect_ = 0.3f, hueFloor_ = 0;
    RW shareRef_{}, shareSrc_{};
    std::vector<float> knots_, curve_;
    double msA_ = 0, msB_ = 0, mrA_ = 0, mrB_ = 0;
    M2 T_{1, 0, 0, 1};
    Vec3 regionOffs_[kRegions];
    float bandA_[3] = {0, 0, 0}, bandB_[3] = {0, 0, 0};
};

}  // namespace

MatchResult matchLook(const std::vector<Vec3>& reference, const std::vector<Vec3>& source, const MatchOptions& opt) {
    Model model(reference, source, opt);
    MatchResult res;
    res.lut = Lut3D(opt.lutSize);
    res.lut.fill([&](Vec3 c) { return oklabToRgb(model.applyLab(rgbToOklab(c))); });
    res.report = model.report();
    return res;
}

// ---------- source-aware match ----------

namespace {

constexpr int kTone = 257;
constexpr float kCanonSigma = 1.6f;     // canonical scene: log2 luminance ~ normal, sigma in stops
constexpr float kCanonChroma = 0.07f;   // canonical RMS OKLab chroma
constexpr float kStopFloor = 1.0f / 4096;
// limits on the tone curve, in stops per stop. A grade that would need more stops short of the reference:
// a steep stretch multiplies skin texture and noise by its slope, a flat one turns skin to wax.
constexpr float kMinCurveSlope = 0.6f, kMaxCurveSlope = 1.6f, kMaxSkinSlope = 1.05f;
constexpr float kMaxExposureMove = 2.0f, kSkinOverGrey = 0.5f;  // stops
constexpr float kMinLightSlope = 0.85f, kMaxLightSlope = 1.15f, kMaxSkinLightSlope = 1.03f;  // the final tone match, in lightness

// Acklam's inverse normal CDF
float invNormal(double p) {
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
    double q, r;
    if (p < 0.02425) {
        q = std::sqrt(-2 * std::log(p));
        return float((((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1));
    }
    if (p > 1 - 0.02425) return -invNormal(1 - p);
    q = p - 0.5; r = q * q;
    return float((((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1));
}

// Monotone piecewise-linear curve in stops, extended past both ends with slope 1, except that a lift
// of the darkest values fades out below them (so true black stays black instead of jumping up).
struct Curve {
    std::vector<float> x, y;
    float operator()(float v) const {
        if (v <= x.front()) {
            const float lift = y.front() - x.front(), k = lift > 0 ? std::min(3.0f, 1 + lift / 3) : 1.0f;
            return y.front() + (v - x.front()) * k;
        }
        if (v >= x.back()) return y.back() + (v - x.back());
        size_t i = std::upper_bound(x.begin(), x.end(), v) - x.begin() - 1;
        float f = (v - x[i]) / std::max(1e-9f, x[i + 1] - x[i]);
        return y[i] + f * (y[i + 1] - y[i]);
    }
};

// Re-integrates y over x with its slope held in [lo, hi], anchored at the middle knot, so a steep
// or flat stretch can't posterise or crush (the same idea as limitSlope for the display curve).
void capSlope(Curve& c, float lo, float hi) {
    const size_t n = c.x.size(), mid = n / 2;
    std::vector<float> y = c.y;
    for (size_t i = mid + 1; i < n; ++i) {
        float dx = c.x[i] - c.x[i - 1];
        y[i] = y[i - 1] + std::clamp(c.y[i] - c.y[i - 1], lo * dx, hi * dx);
    }
    for (size_t i = mid; i-- > 0;) {
        float dx = c.x[i + 1] - c.x[i];
        y[i] = y[i + 1] - std::clamp(c.y[i + 1] - c.y[i], lo * dx, hi * dx);
    }
    c.y = y;
}

float trimmedMean(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    size_t lo = v.size() / 20, hi = v.size() - v.size() / 20;
    double s = 0;
    for (size_t i = lo; i < hi; ++i) s += v[i];
    return hi > lo ? float(s / (hi - lo)) : 0.0f;
}

// Where the look's exposure lands: a reference far from normal exposure (night, high key) keeps
// most of its character, but a normally exposed clip isn't dragged all the way there.
float moderateKey(float k) {
    if (k < -1.0f) return -1.0f + (k + 1.0f) * 0.55f;
    if (k > 0.5f) return 0.5f + (k - 0.5f) * 0.55f;
    return k;
}

inline float rollOff(float y) {  // linear display value, slope 1 below 0.7, asymptote 1
    const float k = 0.7f;
    return y <= k ? y : k + (1 - k) * (1 - std::exp(-(y - k) / (1 - k)));
}

std::vector<float> lumaStops(const std::vector<Vec3>& lin) {
    std::vector<float> s;
    s.reserve(lin.size());
    for (const Vec3& l : lin) s.push_back(std::log2(std::max(luma709(l), kStopFloor) / 0.18f));
    return s;
}

// Tone levels of a frame from its sorted quantiles, counted evenly across the range it occupies: a tone
// counts once however many pixels have it. By pixel count, a plain backdrop (most of a close-up) would own
// the curve, and skin of the same brightness would get whatever stretch the backdrop asked for.
using ToneLevels = std::array<float, 5>;
ToneLevels occupiedLevels(const std::vector<float>& q, float bin) {
    const float lo = q.front(), hi = q.back();
    const int bins = std::max(1, int(std::ceil((hi - lo) / bin)));
    std::vector<float> w(size_t(bins), 0.0f);
    for (float v : q) w[size_t(std::clamp(int((v - lo) / bin), 0, bins - 1))] += 1;
    float total = 0;
    for (float& v : w) { v = std::min(v, 1.0f); total += v; }
    ToneLevels out;
    const float at[5] = {0.02f, 0.25f, 0.5f, 0.75f, 0.98f};
    float sum = 0;
    int i = 0;
    for (int k = 0; k < 5; ++k) {
        const float want = at[k] * total;
        while (i + 1 < bins && sum + w[size_t(i)] < want) sum += w[size_t(i++)];
        out[size_t(k)] = lo + (float(i) + clamp01((want - sum) / std::max(w[size_t(i)], 1e-3f))) * bin;
    }
    return out;
}

// One smooth curve from level pairs: a cubic (least squares through the five), so it can make a gentle S
// with a toe and shoulder, its slope changes evenly and it can't kink. Its bend is scaled back until the
// slope stays in [lo, hi] over the frame's range. Past the range it carries on in a straight line.
// Stored as a fine polyline for Curve.
// Over [keepLo, keepHi] (in `from` units; skin) the slope stays at most keepMax: there a slope is texture.
Curve smoothCurve(const ToneLevels& from, const ToneLevels& to, float lo, float hi, float keepLo = 1, float keepHi = 0, float keepMax = 0) {
    const float c0 = from[2], u0 = from[0] - c0, u1 = from[4] - c0;
    double A[4][5] = {};
    for (int i = 0; i < 5; ++i) {
        const double u = from[size_t(i)] - c0, B[4] = {1, u, u * u, u * u * u};
        for (int r = 0; r < 4; ++r) {
            for (int k = 0; k < 4; ++k) A[r][k] += B[r] * B[k];
            A[r][4] += B[r] * (to[size_t(i)] - from[size_t(i)]);
        }
    }
    A[1][1] += 1e-3; A[2][2] += 1e-3; A[3][3] += 1e-3;  // a frame of one tone: a plain offset
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = A[r][c] / A[c][c];
                for (int k = c; k < 5; ++k) A[r][k] -= f * A[c][k];
            }
    const float a = float(A[0][4] / A[0][0]);  // y - x = a + b u + c u^2 + d u^3
    float b = float(A[1][4] / A[1][1]), c = float(A[2][4] / A[2][2]), d = float(A[3][4] / A[3][3]);
    float top = 0, bot = 0;
    for (int i = 0; i <= 32; ++i) {
        const float u = u0 + (u1 - u0) * float(i) / 32, sl = b + (2 * c + 3 * d * u) * u;
        top = std::max(top, sl); bot = std::min(bot, sl);
    }
    float k = std::min({1.0f, (hi - 1) / std::max(top, 1e-6f), (1 - lo) / std::max(-bot, 1e-6f)});
    if (keepLo < keepHi) {
        float keepTop = 0;
        for (int i = 0; i <= 16; ++i) {
            const float u = keepLo - c0 + (keepHi - keepLo) * float(i) / 16;
            keepTop = std::max(keepTop, b + (2 * c + 3 * d * u) * u);
        }
        k = std::min(k, (keepMax - 1) / std::max(keepTop, 1e-6f));
    }
    b *= k; c *= k; d *= k;
    const float xa = u0 - 14, xb = u1 + 8;
    auto y = [=](float u) {
        const float v = std::clamp(u, u0, u1), sl = b + (2 * c + 3 * d * v) * v;
        return u + a + (b + (c + d * v) * v) * v + (u - v) * sl;
    };
    Curve out;
    const int N = 1024;
    for (int i = 0; i <= N; ++i) {
        const float u = xa + (xb - xa) * float(i) / N;
        out.x.push_back(u + c0);
        out.y.push_back(y(u) + c0);
    }
    return out;
}

// Smooth monotone curve from quantile pairs, for the statistical benchmark methods: 17 control points (every 1/16 of the ranks) joined by a
// monotone cubic (Fritsch-Carlson), then its slope held in [lo, hi]. A curve read straight off the
// quantiles of an 8-bit frame is a staircase (flat where codes repeat, steep across gaps), which turns
// into posterisation and amplified noise; this one is smooth.
Curve smoothMap(const std::vector<float>& from, const std::vector<float>& to, float lo, float hi) {
    const int K = 17, N = int(from.size());
    std::vector<double> X(K), Y(K), d(K - 1), m(K);
    for (int k = 0; k < K; ++k) {
        int i = std::min(N - 1, k * (N - 1) / (K - 1));
        // average a few quantiles around each control point: steadier than a single one
        double sx = 0, sy = 0; int c = 0;
        for (int j = std::max(0, i - 4); j <= std::min(N - 1, i + 4); ++j) { sx += from[j]; sy += to[j]; ++c; }
        X[k] = sx / c; Y[k] = sy / c;
        if (k) { X[k] = std::max(X[k], X[k - 1] + 0.05); Y[k] = std::max(Y[k], Y[k - 1]); }
    }
    for (int k = 0; k < K - 1; ++k) d[k] = (Y[k + 1] - Y[k]) / (X[k + 1] - X[k]);
    m[0] = d[0]; m[K - 1] = d[K - 2];
    for (int k = 1; k < K - 1; ++k) m[k] = d[k - 1] * d[k] <= 0 ? 0 : (d[k - 1] + d[k]) / 2;
    for (int k = 0; k < K - 1; ++k) {
        if (d[k] == 0) { m[k] = m[k + 1] = 0; continue; }
        double a = m[k] / d[k], b = m[k + 1] / d[k], h = a * a + b * b;
        if (h > 9) { double t = 3 / std::sqrt(h); m[k] = t * a * d[k]; m[k + 1] = t * b * d[k]; }
    }
    Curve c;
    const int S = 256;
    for (int i = 0; i <= S; ++i) {
        double x = X[0] + (X[K - 1] - X[0]) * i / S;
        int k = std::min(K - 2, int(std::upper_bound(X.begin(), X.end(), x) - X.begin()) - 1);
        k = std::max(k, 0);
        double h = X[k + 1] - X[k], t = (x - X[k]) / h, t2 = t * t, t3 = t2 * t;
        double y = (2 * t3 - 3 * t2 + 1) * Y[k] + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * Y[k + 1] + (t3 - t2) * h * m[k + 1];
        c.x.push_back(float(x));
        c.y.push_back(float(y));
    }
    capSlope(c, lo, hi);
    return c;
}

// LUT shaper from where the frame's code values are: a third the identity, two thirds their CDF (all
// three channels pooled, smoothed), so the grid is dense where the picture is and never empty elsewhere.
std::vector<float> frameShaper(const Image& img) {
    const float uni = 0.35f;
    const int bins = 1024;
    std::vector<double> h(bins, 0.0);
    for (const Vec3& c : img.px)
        for (int k = 0; k < 3; ++k) h[std::min(bins - 1, int(clamp01(c[k]) * bins))] += 1;
    std::vector<double> sm(bins, 0.0);
    for (int i = 0; i < bins; ++i) {
        double a = 0;
        for (int k = -12; k <= 12; ++k) a += h[std::clamp(i + k, 0, bins - 1)];
        sm[i] = a;
    }
    std::vector<double> cdf(bins + 1, 0.0);
    for (int i = 0; i < bins; ++i) cdf[i + 1] = cdf[i] + sm[i];
    const int n = Lut3D::kShaperSize;
    std::vector<float> s(n);
    for (int i = 0; i < n; ++i) {
        double x = double(i) / (n - 1), pos = x * bins;
        int b = std::min(bins - 1, int(pos));
        double c = (cdf[b] + (pos - b) * sm[b]) / std::max(1.0, cdf[bins]);
        s[i] = float(uni * x + (1 - uni) * c);
    }
    s.front() = 0; s.back() = 1;
    for (int i = 1; i < n; ++i) s[i] = std::max(s[i], s[i - 1] + 1e-5f);
    for (float& v : s) v /= s.back();
    return s;
}

}  // namespace


// ---------- statistical transfer: HM-MKL-HM ----------
// After Pitie & Kokaram, "The linear Monge-Kantorovitch linear colour mapping for example-based colour
// transfer" (2007), and the compound histogram-match / MKL / histogram-match of Grogan et al.:
// T = Ss^-1/2 (Ss^1/2 St Ss^1/2)^1/2 Ss^-1/2 maps a distribution with covariance Ss onto one with St.
// Here it runs in OKLab after the source normalisation, and the histogram matches are on lightness only,
// through the same smooth slope-capped curve as the tone match (a raw CDF match would posterise).
namespace {

using M3 = std::array<std::array<double, 3>, 3>;

M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

// symmetric 3x3 -> V diag(f(eigenvalues)) V^T (Jacobi rotations)
M3 symFunc(M3 a, double (*f)(double)) {
    M3 v{};
    for (int i = 0; i < 3; ++i) v[i][i] = 1;
    for (int sweep = 0; sweep < 50; ++sweep) {
        double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
        if (off < 1e-14) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-18) continue;
                const double th = 0.5 * std::atan2(2 * a[p][q], a[q][q] - a[p][p]);
                const double c = std::cos(th), sn = std::sin(th);
                for (int k = 0; k < 3; ++k) {  // a = J^T a J
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq; a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk; a[q][k] = sn * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - sn * vkq; v[k][q] = sn * vkp + c * vkq;
                }
            }
    }
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += v[i][k] * f(std::max(a[k][k], 1e-9)) * v[j][k];
    return r;
}

void meanCov(const std::vector<Vec3>& px, double mean[3], M3& cov) {
    mean[0] = mean[1] = mean[2] = 0;
    for (const Vec3& c : px) for (int i = 0; i < 3; ++i) mean[i] += c[i];
    for (int i = 0; i < 3; ++i) mean[i] /= std::max<size_t>(1, px.size());
    cov = M3{};
    for (const Vec3& c : px)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov[i][j] += (c[i] - mean[i]) * (c[j] - mean[j]);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) cov[i][j] /= std::max<size_t>(1, px.size());
        cov[i][i] += 1e-6;
    }
}

// lightness match on 257 quantiles through smoothMap (which works in stop-like units: scale by 10)
Curve lightnessMap(const std::vector<Vec3>& from, const std::vector<Vec3>& to) {
    std::vector<float> a, b;
    for (const Vec3& c : from) a.push_back(c.r * 10);
    for (const Vec3& c : to) b.push_back(c.r * 10);
    return smoothMap(evenQuantiles(a, 257), evenQuantiles(b, 257), 0.35f, 2.2f);
}

struct StatTransfer {
    Curve hm1, hm2;
    double ms[3], mt[3];
    M3 T;
    Vec3 mkl(Vec3 c, double l) const {
        double x[3] = {l - ms[0], c.g - ms[1], c.b - ms[2]}, y[3];
        for (int i = 0; i < 3; ++i) y[i] = mt[i] + T[i][0] * x[0] + T[i][1] * x[1] + T[i][2] * x[2];
        return Vec3(float(y[0]), float(y[1]), float(y[2]));
    }
    Vec3 apply(Vec3 lab) const {
        Vec3 o = mkl(lab, hm1(lab.r * 10) / 10);
        o.r = hm2(o.r * 10) / 10;
        return o;
    }
    StatTransfer(const std::vector<Vec3>& src, const std::vector<Vec3>& ref) {
        hm1 = lightnessMap(src, ref);
        std::vector<Vec3> s1 = src;
        for (Vec3& c : s1) c.r = hm1(c.r * 10) / 10;
        M3 cs, ct;
        meanCov(s1, ms, cs);
        meanCov(ref, mt, ct);
        const M3 h = symFunc(cs, [](double v) { return std::sqrt(v); });
        const M3 hi = symFunc(cs, [](double v) { return 1 / std::sqrt(v); });
        T = mul(hi, mul(symFunc(mul(h, mul(ct, h)), [](double v) { return std::sqrt(v); }), hi));
        // identity mean shift is part of the map; lightness is re-matched after it
        std::vector<Vec3> s2;
        for (const Vec3& c : s1) s2.push_back(mkl(c, c.r));
        hm2 = lightnessMap(s2, ref);
    }
};

// The hybrid (method 3): lightness histogram-matched to the reference (gently: the canonical tone
// curve has already done the heavy lifting, so its slope stays in [0.6, 1.6]) and chroma scaled to the
// reference's spread about neutral by one factor (the isotropic case of MKL: an anisotropic map skews
// hue relationships), then the thing-by-thing grade on top for the neutral axis, skin and sectors.
struct ToneChroma {
    Curve hm;
    float k = 1;
    Vec3 apply(Vec3 lab) const { return Vec3(hm(lab.r * 10) / 10, lab.g * k, lab.b * k); }
    ToneChroma(const std::vector<Vec3>& src, const std::vector<Vec3>& ref) {
        std::vector<float> a, b;
        for (const Vec3& c : src) a.push_back(c.r * 10);
        for (const Vec3& c : ref) b.push_back(c.r * 10);
        hm = smoothMap(evenQuantiles(a, 257), evenQuantiles(b, 257), 0.6f, 1.6f);
        double ms = 0, mt = 0;
        for (const Vec3& c : src) ms += c.g * c.g + c.b * c.b;
        for (const Vec3& c : ref) mt += c.g * c.g + c.b * c.b;
        ms /= std::max<size_t>(1, src.size()); mt /= std::max<size_t>(1, ref.size());
        k = float(std::clamp(std::sqrt(mt / std::max(ms, 1e-8)), 0.85, 1.3));
    }
};

}  // namespace

// ---------- the reference's look, translated thing by thing ----------
// Matching global colour statistics after normalisation makes a filter (whites pushed to whatever the
// mean says, skin and everything else split apart). A grade renders each kind of thing its own way,
// so the look is read from the reference per thing and applied per thing:
//   * the neutral axis: the tint of the reference's near-grey pixels per lightness band (its split
//     tone). Greys take exactly that, so whites get the reference's white;
//   * skin: toward the reference's skin hue and saturation, in bounded steps (kept if it has none);
//   * other colours per hue sector, only where the reference has that sector: its hue shift (within
//     15 deg) and saturation; elsewhere just the overall saturation;
//   * colours move relative to the neutral axis by rotation and scaling, so none crosses it.

namespace {

constexpr int kLBands = 5, kSectors = 6;
const float kSectorHue[kSectors] = {25, 85, 145, 205, 265, 325};  // red, yellow, green, cyan, blue, magenta (OKLab deg)

inline float bandW(float L, int k) { float c = 0.1f + 0.2f * k, d = (L - c) / 0.12f; return std::exp(-d * d); }
inline float angDiff(float a, float b) { return std::remainder(a - b, 6.2831853f); }

struct LookProfile {
    float nA[kLBands] = {}, nB[kLBands] = {}, nW[kLBands] = {};  // neutral tint per band
    float skinHue = 0, skinC = 0, skinShare = 0;
    float secHue[kSectors] = {}, secC[kSectors] = {}, secShare[kSectors] = {};
    float sat = 0;               // median chroma of the colourful pixels (relative to the neutral axis)
    float l2 = 0, l50 = 0, l98 = 0;

    Vec3 neutral(float L) const {  // (unused, a, b) tint of grey at lightness L
        float a = 0, b = 0, w = 0;
        for (int k = 0; k < kLBands; ++k) { float g = bandW(L, k) * nW[k]; a += g * nA[k]; b += g * nB[k]; w += g; }
        return w > 1e-6f ? Vec3(0, a / w, b / w) : Vec3(0, 0, 0);
    }

    LookProfile() = default;
    explicit LookProfile(const std::vector<Vec3>& lab) {
        double na[kLBands] = {}, nb[kLBands] = {}, nw[kLBands] = {};
        for (const Vec3& c : lab) {
            float C = std::hypot(c.g, c.b);
            float g = std::exp(-(C / 0.02f) * (C / 0.02f));
            if (g < 0.05f) continue;
            for (int k = 0; k < kLBands; ++k) { float w = g * bandW(c.r, k); na[k] += w * c.g; nb[k] += w * c.b; nw[k] += w; }
        }
        for (int k = 0; k < kLBands; ++k) {
            nW[k] = float(std::min(1.0, nw[k] / (0.01 * lab.size() + 1)));  // confidence: enough grey in the band
            if (nw[k] > 1e-6) { nA[k] = float(na[k] / nw[k]); nB[k] = float(nb[k] / nw[k]); }
        }
        double sx = 0, sy = 0, sc = 0, sw = 0, ex[kSectors] = {}, ey[kSectors] = {}, ec[kSectors] = {}, ew[kSectors] = {};
        std::vector<float> cs;
        size_t colourful = 0;
        for (const Vec3& c : lab) {
            Vec3 n = neutral(c.r);
            float a = c.g - n.g, b = c.b - n.b, C = std::hypot(a, b);
            if (c.r < 0.12f || C < 0.02f) continue;
            cs.push_back(C);
            ++colourful;
            float h = std::atan2(b, a), sk = skinWeight(c);
            if (sk > 0.2f) { sx += sk * std::cos(h); sy += sk * std::sin(h); sc += sk * C; sw += sk; }
            if (C < 0.04f) continue;
            for (int k = 0; k < kSectors; ++k) {
                float d = angDiff(h, kSectorHue[k] * 0.0174533f) / 0.44f, w = std::exp(-d * d) * (1 - sk);
                ex[k] += w * std::cos(h); ey[k] += w * std::sin(h); ec[k] += w * C; ew[k] += w;
            }
        }
        const double total = std::max<double>(1, lab.size());
        skinShare = float(sw / total);
        if (sw > 0) { skinHue = float(std::atan2(sy, sx)); skinC = float(sc / sw); }
        for (int k = 0; k < kSectors; ++k) {
            secShare[k] = float(ew[k] / total);
            if (ew[k] > 0) { secHue[k] = float(std::atan2(ey[k], ex[k])); secC[k] = float(ec[k] / ew[k]); }
        }
        sat = cs.empty() ? 0.0f : evenQuantiles(cs, 3)[1];
        std::vector<float> L;
        for (const Vec3& c : lab) L.push_back(c.r);
        std::vector<float> q = evenQuantiles(L, 51);
        l2 = q[1]; l50 = q[25]; l98 = q[49];
        (void)colourful;
    }

    std::string describe() const {
        std::string s;
        char b[160];
        static const char* bandName[kLBands] = {"blacks", "shadows", "mids", "highlights", "whites"};
        s += "neutral axis (OKLab tint of greys):";
        for (int k = 0; k < kLBands; ++k) {
            if (nW[k] < 0.2f) continue;
            float C = std::hypot(nA[k], nB[k]), h = std::atan2(nB[k], nA[k]) * 57.2958f;
            std::snprintf(b, sizeof b, " %s %.3f @ %.0f deg;", bandName[k], C, h < 0 ? h + 360 : h);
            s += b;
        }
        std::snprintf(b, sizeof b, "\nskin: %.1f%% of the frame, hue %.0f deg, chroma %.3f\nsaturation (median chroma) %.3f; "
                      "lightness 2/50/98%%: %.2f / %.2f / %.2f\nsectors:", 100 * skinShare, skinHue * 57.2958f, skinC, sat, l2, l50, l98);
        s += b;
        static const char* secName[kSectors] = {"red", "yellow", "green", "cyan", "blue", "magenta"};
        for (int k = 0; k < kSectors; ++k) {
            if (secShare[k] < 0.02f) continue;
            float h = secHue[k] * 57.2958f;
            std::snprintf(b, sizeof b, " %s %.0f%% (hue %.0f, chroma %.3f);", secName[k], 100 * secShare[k], h < 0 ? h + 360 : h, secC[k]);
            s += b;
        }
        return s + "\n";
    }
};

// ---------- skin, judged before anything is neutralised ----------
// Skin is recognised on the frame as decoded (no white balance, tone or saturation change yet), with a
// soft weight: warm hues around 52 deg, enough chroma to be a colour, not black or blown. After the
// clip has been neutralised and its saturation evened out, a face no longer looks like skin, so a
// mask taken there turned on and off from one version of a clip to the next.
float skinPrior(const Vec3& lab) {
    const float C = std::hypot(lab.g, lab.b), h = std::atan2(lab.b, lab.g) * 57.29578f;
    const float d = (std::fmod(h - 52.0f + 540.0f, 360.0f) - 180.0f) / 22.0f;
    auto ss = [](float a, float b, float v) { const float t = clamp01((v - a) / (b - a)); return t * t * (3 - 2 * t); };
    return std::exp(-d * d) * ss(0.003f, 0.012f, C) * (1 - ss(0.15f, 0.22f, C)) * ss(0.15f, 0.28f, lab.r) *
           (1 - ss(0.95f, 0.99f, lab.r));
}

struct SkinStats {
    float share = 0, L = 0, C = 0, hue = 0;  // weighted medians (hue: circular mean), radians
    float frameSat = 0, frameL50 = 0;         // the frame's median chroma (colourful pixels) and lightness
};

// weighted median of v (weights w)
float weightedMedian(std::vector<std::pair<float, float>> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double total = 0, acc = 0;
    for (auto& p : v) total += p.second;
    for (auto& p : v) { acc += p.second; if (acc >= total / 2) return p.first; }
    return v.back().first;
}

SkinStats skinStats(const std::vector<Vec3>& lab, const std::vector<float>& w) {
    SkinStats st;
    std::vector<std::pair<float, float>> L, C;
    std::vector<float> sat, all;
    double sx = 0, sy = 0, sw = 0;
    for (size_t i = 0; i < lab.size(); ++i) {
        const Vec3& c = lab[i];
        const float ch = std::hypot(c.g, c.b);
        all.push_back(c.r);
        if (c.r > 0.12f && ch > 0.02f) sat.push_back(ch);
        if (w[i] < 0.05f) continue;
        L.push_back({c.r, w[i]});
        C.push_back({ch, w[i]});
        const float h = std::atan2(c.b, c.g);
        sx += w[i] * std::cos(h); sy += w[i] * std::sin(h); sw += w[i];
    }
    st.share = float(sw / std::max<size_t>(1, lab.size()));
    st.L = weightedMedian(L); st.C = weightedMedian(C);
    st.hue = sw > 0 ? float(std::atan2(sy, sx)) : 0.0f;
    st.frameSat = sat.empty() ? 0.0f : evenQuantiles(sat, 3)[1];
    st.frameL50 = all.empty() ? 0.5f : evenQuantiles(all, 3)[1];
    return st;
}

class Grade {
public:
    Grade(const LookProfile& ref, const LookProfile& src, const SkinStats& refSkin = {}, const SkinStats& srcSkin = {})
        : ref_(ref), src_(src) {
        sat_ = src.sat > 1e-4f ? std::clamp(ref.sat / src.sat, 0.75f, 1.25f) : 1.0f;
        // Skin takes the reference's skin *treatment*, not the reference person's skin: how saturated
        // skin is relative to the rest of the frame (so a pale face stays pale and a deep complexion
        // deep), its hue moved at most 6 deg toward the reference's (and kept within natural skin hues),
        // and never ashy or orange (chroma held in [0.035, 0.09]). Where the tone curve left skin darker,
        // relative to the frame, than the reference's skin sits, it is lifted back (never darkened).
        hasSkin_ = srcSkin.share >= 0.003f && srcSkin.C > 1e-3f;
        if (hasSkin_) {
            const bool refSkin_ = refSkin.share >= 0.02f;
            float k = std::clamp(std::sqrt(sat_), 0.9f, 1.1f);
            if (refSkin_) {
                const float relR = refSkin.C / std::max(refSkin.frameSat, 0.02f);
                const float relS = srcSkin.C / std::max(srcSkin.frameSat, 0.02f);
                k = std::clamp(relR / std::max(relS, 1e-3f), 0.8f, 1.25f);
            }
            const float ct = std::clamp(srcSkin.C * k, 0.035f, 0.09f);
            skinCr_ = std::clamp(ct / srcSkin.C, 0.6f, 4.0f);
            float dh = refSkin_ ? std::clamp(angDiff(refSkin.hue, srcSkin.hue), -0.105f, 0.105f) : 0.0f;
            skinHue_ = std::clamp(srcSkin.hue + dh, 36.0f * 0.0174533f, 64.0f * 0.0174533f);
            skinDh_ = angDiff(skinHue_, srcSkin.hue);
        }
        for (int k = 0; k < kSectors; ++k) {
            ev_[k] = std::min(1.0f, std::min(ref.secShare[k], src.secShare[k]) / 0.03f);
            dh_[k] = std::clamp(angDiff(ref.secHue[k], src.secHue[k]), -0.26f, 0.26f);  // 15 deg
            cr_[k] = src.secC[k] > 1e-4f ? std::clamp(ref.secC[k] / src.secC[k], 0.8f, 1.25f) : sat_;
        }
    }

    // sk: how much this colour is skin (skinPrior of the decoded source colour)
    Vec3 apply(Vec3 lab, float sk = -1) const {
        if (sk < 0) sk = skinWeight(lab);
        if (!hasSkin_) sk = 0;
        Vec3 ns = src_.neutral(lab.r);
        Vec3 nr = ref_.neutral(lab.r);
        {
            // The source's "neutral" tint is only a cast when the frame has real neutrals. In a frame of
            // skin and beige (a warm studio backdrop) the low-chroma pixels are skin-coloured, and taking
            // their tint off would be grey-world: skin would go grey or cross to blue. So a skin-hued
            // neutral estimate counts for less, and none moves the axis by more than 0.018.
            const float cn = std::hypot(ns.g, ns.b);
            if (cn > 1e-5f) {
                const float hd = (std::atan2(ns.b, ns.g) * 57.29578f - 50.0f) / 25.0f;
                float k = 1 - 0.6f * std::exp(-hd * hd);
                k = std::min(k, 0.018f / (cn * k) * k);
                ns.g *= k; ns.b *= k;
            }
        }
        // skin is not a neutral: it keeps its own tint instead of being moved with the grey axis (a cast
        // was already taken out by the white balance, in linear light; the reference's tint of white on
        // skin highlights reads as pink or lavender skin)
        ns.g *= 1 - 0.5f * sk; ns.b *= 1 - 0.5f * sk;
        nr.g *= 1 - 0.9f * sk; nr.b *= 1 - 0.9f * sk;
        float a = lab.g - ns.g, b = lab.b - ns.b;
        const float C = std::hypot(a, b), h = std::atan2(b, a);
        // per hue sector where both frames have it, else the overall saturation
        float dh = 0, cr = 0, w = 0;
        for (int k = 0; k < kSectors; ++k) {
            float d = angDiff(h, kSectorHue[k] * 0.0174533f) / 0.44f, g = std::exp(-d * d);
            dh += g * ev_[k] * dh_[k];
            cr += g * (ev_[k] * cr_[k] + (1 - ev_[k]) * sat_);
            w += g;
        }
        dh /= std::max(w, 1e-6f); cr /= std::max(w, 1e-6f);
        // lips and other strong reds next to skin keep their saturation
        {
            const float d = angDiff(h, 18.0f * 0.0174533f) / (16.0f * 0.0174533f);
            const float lip = std::exp(-d * d) * clamp01((C - 0.05f) / 0.04f) * (1 - sk);
            cr += (std::max(cr, 1.0f) - cr) * lip;
        }
        dh += (skinDh_ - dh) * sk;
        cr += (skinCr_ - cr) * sk;
        dh *= clamp01((C - 0.015f) / 0.035f);  // no hue to turn on a grey
        float h2 = h + dh;
        // skin hues held together: stray red or yellow patches pulled a quarter of the way to the face's hue
        if (sk > 0) h2 += angDiff(skinHue_, h2) * 0.25f * sk * clamp01((C - 0.015f) / 0.02f);
        const float C2 = C * cr;
        return {lab.r, C2 * std::cos(h2) + nr.g, C2 * std::sin(h2) + nr.b};
    }

private:
    const LookProfile ref_, src_;
    float sat_ = 1, skinDh_ = 0, skinCr_ = 1, skinHue_ = 0;
    bool hasSkin_ = false;
    float ev_[kSectors] = {}, dh_[kSectors] = {}, cr_[kSectors] = {};
};

}  // namespace

std::string describeLook(const Image& img) {
    std::vector<Vec3> lab;
    for (const Vec3& c : downsample(img, 480).px) lab.push_back(rgbToOklab(clamp01(c)));
    return LookProfile(lab).describe();
}

namespace {

// ---------- the reference, as the match reads it ----------
// Everything the match takes from a reference still: its analysis (for the report), its luminance
// distribution in stops (where the tone lands), its look profile (neutral axis, skin, sectors,
// saturation), its skin treatment, its lightness levels (final tone match) and its pixels (the
// statistical methods only).
struct RefLook {
    SceneAnalysis analysis;
    std::vector<float> stops;  // kTone quantiles of log2(luminance / 0.18)
    ToneLevels toneLevels{}, lightLevels{};  // occupied levels of stops and of lightness x 10
    float key = 0;             // trimmed mean of the stops
    LookProfile profile;
    SkinStats skin;
    std::vector<float> lq;     // 257 quantiles of OKLab L x 10
    std::vector<Vec3> lab;
};

RefLook makeRefLook(const Image& refIn) {
    RefLook r;
    const Image ref = downsample(cropBars(refIn), 480);
    AnalysisOptions ro;
    ro.transfer = int(Transfer::Rec709);
    r.analysis = analyseScene(ref, ro);
    r.analysis.transferFromUser = false;
    std::vector<Vec3> lin(ref.px.size());
    for (size_t i = 0; i < ref.px.size(); ++i) lin[i] = decodeToRec709(Transfer::Rec709, clamp01(ref.px[i]));
    std::vector<float> st = lumaStops(lin);
    r.key = trimmedMean(st);
    r.stops = evenQuantiles(std::move(st), kTone);
    for (const Vec3& v : ref.px) r.lab.push_back(rgbToOklab(clamp01(v)));
    r.profile = LookProfile(r.lab);
    std::vector<float> w, L;
    for (const Vec3& c : r.lab) { w.push_back(skinPrior(c)); L.push_back(c.r * 10); }
    r.skin = skinStats(r.lab, w);
    r.lq = evenQuantiles(std::move(L), 257);
    r.toneLevels = occupiedLevels(r.stops, 0.25f);
    r.lightLevels = occupiedLevels(r.lq, 0.25f);
    return r;
}

float median(std::vector<float> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5f * (v[m - 1] + v[m]);
}
// median of angles (radians): the median of their offsets from the circular mean
float medianAngle(const std::vector<float>& a) {
    if (a.empty()) return 0;
    double sx = 0, sy = 0;
    for (float x : a) { sx += std::cos(x); sy += std::sin(x); }
    const float c = float(std::atan2(sy, sx));
    std::vector<float> d;
    for (float x : a) d.push_back(angDiff(x, c));
    return c + median(d);
}

// Several stills of one look combined element by element, by median: one odd still (a different
// light, a frame that is mostly one colour) moves the look less than a mean or pooled pixels would.
// Elements a still doesn't have (no skin, no greys in a band, no pixels in a hue sector) are left
// out of that element's median.
RefLook combineRefLooks(const std::vector<RefLook>& rs) {
    if (rs.size() == 1) return rs[0];
    RefLook out;
    auto med = [&](auto get) { std::vector<float> v; for (const RefLook& r : rs) v.push_back(get(r)); return median(v); };
    out.analysis = rs[0].analysis;
    out.analysis.exposure = med([](const RefLook& r) { return r.analysis.exposure; });
    out.analysis.cct = med([](const RefLook& r) { return r.analysis.cct; });
    out.analysis.tint = med([](const RefLook& r) { return r.analysis.tint; });
    out.analysis.contrast = med([](const RefLook& r) { return r.analysis.contrast; });
    out.analysis.saturation = med([](const RefLook& r) { return r.analysis.saturation; });
    out.key = med([](const RefLook& r) { return r.key; });
    out.stops.resize(rs[0].stops.size());
    for (size_t i = 0; i < out.stops.size(); ++i) out.stops[i] = med([i](const RefLook& r) { return r.stops[i]; });
    for (size_t i = 0; i < out.toneLevels.size(); ++i) {
        out.toneLevels[i] = med([i](const RefLook& r) { return r.toneLevels[i]; });
        out.lightLevels[i] = med([i](const RefLook& r) { return r.lightLevels[i]; });
    }
    out.lq.resize(rs[0].lq.size());
    for (size_t i = 0; i < out.lq.size(); ++i) out.lq[i] = med([i](const RefLook& r) { return r.lq[i]; });
    for (const RefLook& r : rs) out.lab.insert(out.lab.end(), r.lab.begin(), r.lab.end());
    // look profile
    LookProfile& p = out.profile;
    for (int k = 0; k < kLBands; ++k) {
        std::vector<float> a, b, w;
        for (const RefLook& r : rs) {
            w.push_back(r.profile.nW[k]);
            if (r.profile.nW[k] > 0.05f) { a.push_back(r.profile.nA[k]); b.push_back(r.profile.nB[k]); }
        }
        p.nW[k] = median(w);
        p.nA[k] = median(a); p.nB[k] = median(b);
    }
    {
        std::vector<float> h, c, sh;
        for (const RefLook& r : rs) {
            sh.push_back(r.profile.skinShare);
            if (r.profile.skinShare >= 0.02f) { h.push_back(r.profile.skinHue); c.push_back(r.profile.skinC); }
        }
        p.skinShare = median(sh); p.skinHue = medianAngle(h); p.skinC = median(c);
    }
    for (int k = 0; k < kSectors; ++k) {
        std::vector<float> h, c, sh;
        for (const RefLook& r : rs) {
            sh.push_back(r.profile.secShare[k]);
            if (r.profile.secShare[k] > 0.005f) { h.push_back(r.profile.secHue[k]); c.push_back(r.profile.secC[k]); }
        }
        p.secShare[k] = median(sh); p.secHue[k] = medianAngle(h); p.secC[k] = median(c);
    }
    p.sat = med([](const RefLook& r) { return r.profile.sat; });
    p.l2 = med([](const RefLook& r) { return r.profile.l2; });
    p.l50 = med([](const RefLook& r) { return r.profile.l50; });
    p.l98 = med([](const RefLook& r) { return r.profile.l98; });
    // skin treatment
    {
        std::vector<float> L, C, h, fs, fl, sh;
        for (const RefLook& r : rs) {
            sh.push_back(r.skin.share); fs.push_back(r.skin.frameSat); fl.push_back(r.skin.frameL50);
            if (r.skin.share >= 0.02f) { L.push_back(r.skin.L); C.push_back(r.skin.C); h.push_back(r.skin.hue); }
        }
        out.skin.share = median(sh); out.skin.frameSat = median(fs); out.skin.frameL50 = median(fl);
        out.skin.L = median(L); out.skin.C = median(C); out.skin.hue = medianAngle(h);
        // the skin treatment is relative to the frame: take it from the stills that have skin
        if (!L.empty()) {
            std::vector<float> fs2, fl2;
            for (const RefLook& r : rs) if (r.skin.share >= 0.02f) { fs2.push_back(r.skin.frameSat); fl2.push_back(r.skin.frameL50); }
            out.skin.frameSat = median(fs2); out.skin.frameL50 = median(fl2);
        }
    }
    return out;
}

}  // namespace

SceneMatch matchScene(const Image& reference, const Image& source, const SceneMatchOptions& o) {
    return matchScene(std::vector<Image>{reference}, source, o);
}

namespace {

SceneMatch matchSceneLook(const RefLook& rl, const Image& srcIn, const SceneMatchOptions& o) {
    SceneMatch res;
    const Image src = downsample(cropBars(srcIn), 480);
    AnalysisOptions ao;
    ao.transfer = o.transfer;
    res.source = analyseScene(src, ao);
    res.source.transferGuess = o.transferGuess;
    res.reference = rl.analysis;
    const Transfer tf = res.source.transfer;
    const float ce = clamp01(o.exposure), cw = clamp01(o.whiteBalance), cc = clamp01(o.contrast), ls = clamp01(o.lookStrength);
    const size_t n = src.px.size();

    std::vector<float> canon(kTone);
    for (int i = 0; i < kTone; ++i) canon[i] = kCanonSigma * invNormal((i + 0.5) / kTone);

    // ---- 1. decode, and white balance in scene linear, where a camera's cast is a plain gain
    // (Bradford, from the analysis' near-neutral estimate). How far depends on how neutral those
    // pixels really are: a scene with nothing grey in it (a blue-hour sky, a red-lit stage) keeps its
    // colour instead of having it taken for a cast. The measure doesn't change with a cast. ----
    float wbConfidence = clamp01(res.source.neutralShare / 0.2f);
    {
        // An estimate that is warm AND magenta sits where skin and beige do, not where light sources do
        // (they lie along the Planckian locus): in a close-up against a warm backdrop the "most neutral"
        // pixels are skin and backdrop, and neutralising them turns a face grey and its lashes cyan, and the
        // skin's own colour variation shows as grey flecks. Hardly anything of such a cast is taken out.
        auto ramp = [](float a, float b, float x) { return clamp01((x - a) / (b - a)); };
        const float skinLike = ramp(3.0f, 10.0f, res.source.tint) * ramp(5600.0f, 4600.0f, res.source.cct);
        wbConfidence *= 1 - 0.95f * skinLike;
    }
    const float cwe = cw * wbConfidence;
    const Mat3 wb = adaptation(kD65x + (res.source.whiteX - kD65x) * cwe, kD65y + (res.source.whiteY - kD65y) * cwe, kD65x, kD65y);
    auto linear = [tf, wb](Vec3 code) {
        Vec3 l = wb * decodeToRec709(tf, code);
        return Vec3(std::max(l.r, 0.0f), std::max(l.g, 0.0f), std::max(l.b, 0.0f));
    };
    std::vector<Vec3> lin0(n);
    for (size_t i = 0; i < n; ++i) lin0[i] = linear(src.px[i]);

    // ---- 2. tone: the camera's decode, then one smooth curve on luminance, the same for R, G and B as a
    // ratio (colour and hue are the frame's own). It is fitted to the tones the frame occupies, so it takes
    // out a grade's or a transfer's curve without reshaping the picture level by level. ----
    std::vector<float> st0 = lumaStops(lin0);
    std::vector<float> s0 = evenQuantiles(std::move(st0), kTone);
    // A sliver of pure black the bar detection can't catch (a logo plate, a burnt-in border) must not read
    // as stops of real shadow: nothing further than 6 stops below the median counts as tone.
    const float toneFloor = s0[kTone / 2] - 6.0f;
    for (float& v : s0) v = std::max(v, toneFloor);
    auto byLuminance = [](const Curve& g, Vec3 l) {
        // below the floor, fill up to it with grey, so black meets the curve's floor continuously
        const float y0 = luma709(l);
        if (y0 < kStopFloor) l = l + Vec3(1, 1, 1) * (kStopFloor - std::max(y0, 0.0f));
        const float y = std::max(luma709(l), kStopFloor);
        return l * (0.18f * std::exp2(g(std::log2(y / 0.18f))) / y);
    };

    // ---- 3. where the tone lands: the reference's distribution, its key moderated ----
    const float mr = rl.key, mr2 = moderateKey(mr);
    ToneLevels refLevels = rl.toneLevels;
    const float refMedian = rl.stops[kTone / 2] + (mr2 - mr);
    if (mr2 > mr) {  // lift a dark reference: keep its highlights, open its shadows
        const float top = rl.stops[kTone * 99 / 100], k = std::clamp((top - mr2) / std::max(1e-3f, top - mr), 0.3f, 1.0f);
        for (float& v : refLevels) v = top - (top - v) * k;
    } else if (mr2 < mr) {  // tame a bright reference: keep its blacks
        const float bot = rl.stops[kTone / 100], k = std::clamp((mr2 - bot) / std::max(1e-3f, mr - bot), 0.3f, 1.0f);
        for (float& v : refLevels) v = bot + (v - bot) * k;
    }

    // target levels: canonical (or the source's own shape, by `contrast`), exposure kept by (1 - `exposure`),
    // then the reference's on top by `lookStrength`
    const ToneLevels srcLevels = occupiedLevels(s0, 0.25f), canonLevels = occupiedLevels(canon, 0.25f);
    const float keep = (1 - ce) * srcLevels[2];
    ToneLevels t;
    for (size_t i = 0; i < t.size(); ++i) {
        const float q = (srcLevels[i] - ce * srcLevels[2]) + (canonLevels[i] + keep - (srcLevels[i] - ce * srcLevels[2])) * cc;
        t[i] = q + (refLevels[i] + keep - q) * ls;
    }
    Curve g1 = smoothCurve(srcLevels, t, kMinCurveSlope, kMaxCurveSlope);

    // Skin, in source stops: contrast comes from the toe and shoulder around it, not from steepening skin,
    // where a slope above 1 reads as crunch. Exposure then is one global gain on top of the curve: skin, where
    // there is any, lands where the reference's skin sits (or a little above its mid grey, if it shows none); a
    // frame without skin puts its mid grey on the reference's. The tonal range alone would leave a subject
    // against a bright sky or wall dark.
    float anchorStops = s0[kTone / 2], targetStops = refMedian;
    {
        std::vector<std::pair<float, float>> skinStops;
        for (size_t i = 0; i < n; ++i) {
            const Vec3 l = byLuminance(g1, lin0[i]);
            const float m = std::max({l.r, l.g, l.b, kStopFloor}), k = rollOff(m) / m;
            const Vec3 lab = rgbToOklab(Vec3(rec709Encode(clamp01(l.r * k)), rec709Encode(clamp01(l.g * k)), rec709Encode(clamp01(l.b * k))));
            const float w = skinPrior(lab);
            if (w > 0.3f && std::hypot(lab.g, lab.b) > 0.025f) skinStops.push_back({std::log2(std::max(luma709(lin0[i]), kStopFloor) / 0.18f), w});
        }
        if (skinStops.size() > n / 100) {
            anchorStops = weightedMedian(skinStops);
            targetStops = rl.skin.share >= 0.02f ? std::log2(std::max(rl.skin.L * rl.skin.L * rl.skin.L, 1e-4f) / 0.18f) + (mr2 - mr)
                                                 : refMedian + kSkinOverGrey;
            std::sort(skinStops.begin(), skinStops.end());
            const float a = skinStops[skinStops.size() / 10].first, b = skinStops[skinStops.size() * 9 / 10].first;
            g1 = smoothCurve(srcLevels, t, kMinCurveSlope, kMaxCurveSlope, a, b, kMaxSkinSlope);
        }
    }
    const float gainStops = std::clamp(targetStops - g1(anchorStops), 0.0f, kMaxExposureMove) * ce * ls;  // lifts only: the fit is never too bright
    for (float& y : g1.y) y += gainStops;

    auto canonicalDisplay = [linear, byLuminance, g1](Vec3 code) {
        Vec3 lc = byLuminance(g1, linear(code));
        // highlights: roll the largest channel off and scale the others with it, so warm skin keeps its colour
        // up to the top (rolling luminance off would leave its red over range, and pulling that back toward
        // grey would hand the highlight to the neutral axis). Only what is far over range goes to white.
        const float mx = std::max({lc.r, lc.g, lc.b, kStopFloor}), m2 = rollOff(mx);
        lc = lc * (m2 / mx);
        const float w = clamp01((mx - 1) / 2), toWhite = w * w * (3 - 2 * w);
        lc = lc + (Vec3(m2, m2, m2) - lc) * toWhite;
        return Vec3(rec709Encode(clamp01(lc.r)), rec709Encode(clamp01(lc.g)), rec709Encode(clamp01(lc.b)));
    };

    // ---- 4. chroma, in OKLab of the canonical frame:
    // (a) what is left of a cast (a grade's colour correction), as an ab offset that may vary with
    //     lightness, fitted robustly so the most coloured pixels count least (a red dress or a blue sky
    //     doesn't read as a cast);
    // (b) the overall saturation brought to a canonical level (by `contrast`), from the frame's more
    //     colourful pixels, bounded so a nearly grey frame isn't pushed into false colour. ----
    std::vector<Vec3> canonLab(n);
    for (size_t i = 0; i < n; ++i) canonLab[i] = rgbToOklab(canonicalDisplay(src.px[i]));
    // Skin weights, taken here: after the scene-linear white balance and the tone normalisation (so a
    // cast, an exposure or a curve doesn't move a face in or out of the mask) but before the residual
    // cast and saturation steps and the look (where a face stops looking like skin).
    auto preSkin = [canonicalDisplay](Vec3 code) { return skinPrior(rgbToOklab(canonicalDisplay(code))); };
    std::vector<float> srcSkinW(n);
    double skinSum = 0;
    for (size_t i = 0; i < n; ++i) { srcSkinW[i] = skinPrior(canonLab[i]); skinSum += srcSkinW[i]; }
    // a frame that is mostly skin (a close-up) has no grey to judge a residual cast by: take out only part
    const float skinFrame = 1 - 0.4f * clamp01((float(skinSum / std::max<size_t>(1, n)) - 0.25f) / 0.4f);
    float offA[2] = {0, 0}, offB[2] = {0, 0};  // ab offset = off[0] + off[1] * (L - 0.5)
    for (int it = 0; it < 5; ++it) {
        std::vector<float> d;
        d.reserve(n);
        for (const Vec3& c : canonLab) {
            float u = c.r - 0.5f;
            d.push_back(std::hypot(c.g - offA[0] - offA[1] * u, c.b - offB[0] - offB[1] * u));
        }
        const float dm = std::max(1e-4f, evenQuantiles(d, 3)[1]);
        double w0 = 0, w1 = 0, w2 = 0, sa = 0, sau = 0, sb = 0, sbu = 0;
        for (size_t i = 0; i < n; ++i) {
            const Vec3& c = canonLab[i];
            if (c.r < 0.12f || c.r > 0.95f) continue;
            const float q = d[i] / dm, w = (1 - 0.6f * srcSkinW[i]) / (1.0f + q * q * q * q), u = c.r - 0.5f;
            w0 += w; w1 += w * u; w2 += w * u * u; sa += w * c.g; sau += w * c.g * u; sb += w * c.b; sbu += w * c.b * u;
        }
        const double ridge = w2 + 0.05 * w0, det = w0 * ridge - w1 * w1;  // ridge on the slope
        if (w0 < 50 || std::abs(det) < 1e-12) break;
        offA[0] = float((ridge * sa - w1 * sau) / det); offA[1] = float((w0 * sau - w1 * sa) / det);
        offB[0] = float((ridge * sb - w1 * sbu) / det); offB[1] = float((w0 * sbu - w1 * sb) / det);
    }
    // only a residual cast, and only as far as the frame has neutrals to judge it by
    for (float* off : {offA, offB}) {
        off[0] = std::clamp(off[0], -0.03f, 0.03f) * cw * wbConfidence * skinFrame;
        off[1] = std::clamp(off[1], -0.06f, 0.06f) * cw * wbConfidence * skinFrame;
    }
    std::vector<float> chromas;
    for (Vec3& c : canonLab) {
        const float u = c.r - 0.5f;
        c.g -= offA[0] + offA[1] * u;
        c.b -= offB[0] + offB[1] * u;
        if (c.r > 0.2f && c.r < 0.9f) chromas.push_back(std::hypot(c.g, c.b));
    }
    // 75th percentile of chroma; for the canonical distribution (Rayleigh, RMS kCanonChroma) it is 1.2x the RMS
    const float srcChroma = chromas.size() > 20 ? evenQuantiles(chromas, 5)[3] / 1.2f : kCanonChroma;
    const float satScale = 1 + (std::clamp(kCanonChroma / std::max(srcChroma, 1e-4f), 0.6f, 1.4f) - 1) * cc;
    auto canonical = [canonicalDisplay, offA, offB, satScale](Vec3 code) {
        Vec3 lab = rgbToOklab(canonicalDisplay(code));
        const float u = lab.r - 0.5f;
        lab.g -= offA[0] + offA[1] * u;
        lab.b -= offB[0] + offB[1] * u;
        // skin keeps its own saturation more than the rest: a face pushed to a canonical saturation
        // goes orange or grey
        const float k = satScale > 1 ? 1 + (satScale - 1) * (1 - 0.7f * skinWeight(lab)) : satScale;
        lab.g *= k;
        lab.b *= k;
        return rgbToOklab(oklabToRgbCompressed(lab));  // in gamut
    };
    std::vector<Vec3> canonRgb(n);
    for (size_t i = 0; i < n; ++i) canonRgb[i] = oklabToRgb(canonical(src.px[i]));

    // ---- 5. the reference's look on the canonical frame (tone already done), thing by thing ----
    std::vector<Vec3> canonLabs;
    for (const Vec3& v : canonRgb) canonLabs.push_back(rgbToOklab(v));
    const std::vector<Vec3>& refLab = rl.lab;
    const LookProfile& refLook = rl.profile;
    const LookProfile srcLook(canonLabs);
    const SkinStats& refSkin = rl.skin;
    const SkinStats srcSkin = skinStats(canonLabs, srcSkinW);
    auto grade = std::make_shared<Grade>(refLook, srcLook, refSkin, srcSkin);
    std::shared_ptr<StatTransfer> stat;
    if (o.method == 1 || o.method == 2) stat = std::make_shared<StatTransfer>(canonLabs, refLab);
    std::shared_ptr<ToneChroma> tc;
    if (o.method == 3) {
        tc = std::make_shared<ToneChroma>(canonLabs, refLab);
        std::vector<Vec3> mapped;
        for (const Vec3& c : canonLabs) mapped.push_back(rgbToOklab(oklabToRgb(tc->apply(c))));
        grade = std::make_shared<Grade>(refLook, LookProfile(mapped), refSkin, skinStats(mapped, srcSkinW));
    }
    const int method = o.method;
    auto graded = [canonical, grade, stat, tc, ls, method](Vec3 code, float sk) {
        Vec3 lab = canonical(code);
        Vec3 look = method == 0 ? grade->apply(lab, sk)
                  : method == 1 ? stat->apply(lab)
                  : method == 2 ? lerp(grade->apply(lab, sk), stat->apply(lab), 0.5f)
                                : grade->apply(rgbToOklab(oklabToRgb(tc->apply(lab))), sk);
        return lerp(lab, look, ls);
    };

    // ---- 6. final tone match: the graded result's occupied levels brought to the reference's with the same
    // kind of smooth curve, on lightness alone (hues and chroma untouched). Skin is never darkened by more
    // than a third of what the curve asks, and no level moves more than 0.15; both limits are smooth. ----
    const float mc = clamp01(o.matchContrast) * ls;
    Curve tone;
    float skinL = -1;  // lightness skin lands at after the look (-1: no skin)
    float lightMid = 0.5f;  // median lightness after the look
    if (mc > 0) {
        std::vector<float> a, b;
        std::vector<std::pair<float, float>> sl;
        for (size_t i = 0; i < n; ++i) {
            const Vec3 g = graded(src.px[i], srcSkinW[i]);
            a.push_back(g.r * 10);
            // clearly coloured skin only: warm whites (walls, sheets) share skin's hue
            if (srcSkinW[i] > 0.3f && std::hypot(g.g, g.b) > 0.025f) sl.push_back({g.r, srcSkinW[i]});
        }
        const std::vector<float> graded257 = evenQuantiles(a, 257);
        lightMid = graded257[128] / 10;
        if (srcSkin.share >= 0.003f && sl.size() > n / 200) skinL = weightedMedian(sl);
        tone = skinL >= 0 ? smoothCurve(occupiedLevels(graded257, 0.25f), rl.lightLevels, kMinLightSlope, kMaxLightSlope, (skinL - 0.15f) * 10, (skinL + 0.15f) * 10, kMaxSkinLightSlope)
                          : smoothCurve(occupiedLevels(graded257, 0.25f), rl.lightLevels, kMinLightSlope, kMaxLightSlope);
    }
    // the curve shapes the levels around the anchor; where the anchor sits is exposure's business
    const float toneAt = skinL >= 0 ? skinL : tone.x.empty() ? 0.0f : lightMid, toneShift = mc > 0 ? tone(toneAt * 10) / 10 - toneAt : 0.0f;
    auto full = [graded, preSkin, tone, mc, skinL, toneShift](Vec3 code) {
        const float sk = preSkin(code);
        Vec3 lab = graded(code, sk);
        if (mc > 0) {
            float d = 0.15f * std::tanh((tone(lab.r * 10) / 10 - lab.r - toneShift) / 0.15f);
            // Skin is not crushed: around the lightness skin sits at, the curve may darken by at most 0.02.
            // The limit is a function of lightness alone, joined softly, so lightness stays smooth:
            // a per-colour skin weight here would bend the grey axis.
            if (skinL >= 0) {
                const float u = (lab.r - skinL) / 0.12f, lim = -(0.15f - 0.13f * std::exp(-u * u)), g = d - lim;
                d = lim + 0.5f * (g + std::sqrt(g * g + 1e-4f));
            }
            lab.r += d * mc;
        }
        return oklabToRgbCompressed(lab, true);
    };
    res.lut = Lut3D(o.look.lutSize);
    res.lut.setShaper(frameShaper(src));
    res.lut.fill(full);
    res.exact = full;

    Image after = src;
    for (Vec3& v : after.px) v = res.lut.apply(v);
    AnalysisOptions after709;
    after709.transfer = int(Transfer::Rec709);
    res.result = analyseScene(after, after709);
    res.result.transferFromUser = false;

    char buf[400];
    std::snprintf(buf, sizeof buf,
                  "source %s (log %.2f), exposure %+.2f stops, %.0f K, tint %+.1f, contrast %.1f stops\n"
                  "reference exposure %+.2f, %.0f K, tint %+.1f, contrast %.1f; key lands at %+.2f\n",
                  transferShortName(res.source.transfer), res.source.logScore, res.source.exposure, res.source.cct,
                  res.source.tint, res.source.contrast, res.reference.exposure, res.reference.cct, res.reference.tint,
                  res.reference.contrast, mr2);
    res.report = std::string(buf) + "reference look:\n" + refLook.describe() + "source (canonical):\n" + srcLook.describe();
    return res;
}

}  // namespace

SceneMatch matchScene(const std::vector<Image>& references, const Image& source, const SceneMatchOptions& o) {
    std::vector<RefLook> rs;
    for (const Image& r : references) if (r.width > 0 && r.height > 0) rs.push_back(makeRefLook(r));
    if (rs.empty()) rs.push_back(makeRefLook(source));  // no reference: the clip's own look
    return matchSceneLook(combineRefLooks(rs), source, o);
}

}  // namespace lm
