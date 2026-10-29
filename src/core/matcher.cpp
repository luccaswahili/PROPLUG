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
    float gc = clamp01((C - 0.015f) / 0.02f) * clamp01((0.20f - C) / 0.05f);
    float gl = clamp01((L - 0.2f) / 0.1f) * clamp01((0.95f - L) / 0.05f);
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
    Model(const std::vector<Vec3>& refRgb, const std::vector<Vec3>& srcRgb, const MatchOptions& opt) : rs_(opt.regionStrength) {
        Stats R = analyse(refRgb, opt.balance), S = analyse(srcRgb, opt.balance);
        shareRef_ = R.share; shareSrc_ = S.share;
        skinProtect_ = opt.skinProtect >= 0 ? opt.skinProtect : (R.share[kSkin] >= kMinShare ? 0.3f : 0.8f);

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
        auto wcov = [](const Stats& st, double& ma, double& mb) {
            double sw = 0; ma = mb = 0;
            for (size_t i = 0; i < st.lab.size(); ++i) { sw += st.w[i]; ma += st.w[i] * st.lab[i].g; mb += st.w[i] * st.lab[i].b; }
            ma /= sw; mb /= sw;
            S2 c{0, 0, 0};
            for (size_t i = 0; i < st.lab.size(); ++i) {
                double da = st.lab[i].g - ma, db = st.lab[i].b - mb;
                c.a += st.w[i] * da * da; c.b += st.w[i] * da * db; c.c += st.w[i] * db * db;
            }
            c.a = c.a / sw + 1e-6; c.b /= sw; c.c = c.c / sw + 1e-6;
            return c;
        };
        S2 cs = wcov(S, msA_, msB_), cr = wcov(R, mrA_, mrB_);
        M2 csh = sqrtPsd(cs), cshi = inv(csh);
        M2 mid = mul(mul(csh, M2{cr.a, cr.b, cr.b, cr.c}), csh);
        M2 midS = sqrtPsd({mid.m00, 0.5 * (mid.m01 + mid.m10), mid.m11});
        M2 T = mul(mul(cshi, midS), cshi);
        // cap how much chroma spread is scaled along each axis
        T_ = eigMap({T.m00, 0.5 * (T.m01 + T.m10), T.m11},
                    [](double l) { return std::clamp(l, kMinChromaGain, kMaxChromaGain); });

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
        for (int r = 0; r < kRegions; ++r) out = out + regionOffs_[r] * (rs_ * w[r]);
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
    float rs_, skinProtect_ = 0.3f;
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

void smooth(std::vector<float>& v, int r) {
    std::vector<float> o(v.size());
    for (int i = 0; i < int(v.size()); ++i) {
        float s = 0;
        for (int k = -r; k <= r; ++k) s += v[std::clamp(i + k, 0, int(v.size()) - 1)];
        o[i] = s / (2 * r + 1);
    }
    v = o;
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

inline float softClip(float y) {  // linear display luminance, slope 1 below 0.9, asymptote 1
    const float k = 0.9f;
    return y <= k ? y : k + (1 - k) * (1 - std::exp(-(y - k) / (1 - k)));
}



std::vector<float> lumaStops(const std::vector<Vec3>& lin) {
    std::vector<float> s;
    s.reserve(lin.size());
    for (const Vec3& l : lin) s.push_back(std::log2(std::max(luma709(l), kStopFloor) / 0.18f));
    return s;
}

Curve quantileMap(const std::vector<float>& from, const std::vector<float>& to, float lo, float hi, int smoothR) {
    Curve c;
    c.x = from;
    for (size_t i = 1; i < c.x.size(); ++i) c.x[i] = std::max(c.x[i], c.x[i - 1] + 1e-4f);
    c.y = to;
    smooth(c.y, smoothR);
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

SceneMatch matchScene(const Image& refIn, const Image& srcIn, const SceneMatchOptions& o) {
    SceneMatch res;
    const Image ref = downsample(refIn, 480), src = downsample(srcIn, 480);
    AnalysisOptions ao;
    ao.transfer = o.transfer;
    res.source = analyseScene(src, ao);
    AnalysisOptions ro;
    ro.transfer = int(Transfer::Rec709);
    res.reference = analyseScene(ref, ro);
    res.reference.transferFromUser = false;
    const Transfer tf = res.source.transfer;
    const float ce = clamp01(o.exposure), cw = clamp01(o.whiteBalance), cc = clamp01(o.contrast), ls = clamp01(o.lookStrength);
    const size_t n = src.px.size();

    std::vector<float> canon(kTone);
    for (int i = 0; i < kTone; ++i) canon[i] = kCanonSigma * invNormal((i + 0.5) / kTone);

    // ---- 1. decode, and white balance in scene linear, where a camera's cast is a plain gain
    // (Bradford, from the analysis' near-neutral estimate). How far depends on how neutral those
    // pixels really are: a scene with nothing grey in it (a blue-hour sky, a red-lit stage) keeps its
    // colour instead of having it taken for a cast. The measure doesn't change with a cast. ----
    const float wbConfidence = clamp01(res.source.neutralShare / 0.2f);
    const float cwe = cw * wbConfidence;
    const Mat3 wb = adaptation(kD65x + (res.source.whiteX - kD65x) * cwe, kD65y + (res.source.whiteY - kD65y) * cwe, kD65x, kD65y);
    auto linear = [tf, wb](Vec3 code) {
        Vec3 l = wb * decodeToRec709(tf, code);
        return Vec3(std::max(l.r, 0.0f), std::max(l.g, 0.0f), std::max(l.b, 0.0f));
    };
    std::vector<Vec3> lin0(n);
    for (size_t i = 0; i < n; ++i) lin0[i] = linear(src.px[i]);

    // ---- 2. canonical tone: the luminance distribution mapped by rank to a fixed one, the same curve
    // for R, G and B as a luminance ratio (colour and hue are the frame's own). Any curve a grade or a
    // transfer applied keeps the order of the pixels' brightness, so this undoes it. ----
    std::vector<float> st0 = lumaStops(lin0);
    const float key0 = trimmedMean(st0);
    const std::vector<float> s0 = evenQuantiles(std::move(st0), kTone);
    const Curve g1 = quantileMap(s0, canon, 0.33f, 3.0f, 3);
    auto byLuminance = [](const Curve& g, Vec3 l) {
        // below the floor, fill up to it with grey, so black meets the curve's floor continuously
        const float y0 = luma709(l);
        if (y0 < kStopFloor) l = l + Vec3(1, 1, 1) * (kStopFloor - std::max(y0, 0.0f));
        const float y = std::max(luma709(l), kStopFloor);
        return l * (0.18f * std::exp2(g(std::log2(y / 0.18f))) / y);
    };
    std::vector<Vec3> lin1(n);
    for (size_t i = 0; i < n; ++i) lin1[i] = byLuminance(g1, lin0[i]);
    const std::vector<float> s1 = evenQuantiles(lumaStops(lin1), kTone);

    // ---- 3. where the tone lands: the reference's distribution, its key moderated ----
    std::vector<Vec3> refLin(ref.px.size());
    for (size_t i = 0; i < ref.px.size(); ++i) refLin[i] = decodeToRec709(Transfer::Rec709, clamp01(ref.px[i]));
    std::vector<float> refStops = lumaStops(refLin);
    const float mr = trimmedMean(refStops), mr2 = moderateKey(mr);
    std::vector<float> r = evenQuantiles(std::move(refStops), kTone);
    if (mr2 > mr) {  // lift a dark reference: keep its highlights, open its shadows
        const float top = r[kTone * 99 / 100], k = std::clamp((top - mr2) / std::max(1e-3f, top - mr), 0.3f, 1.0f);
        for (float& v : r) v = top - (top - v) * k;
    } else if (mr2 < mr) {  // tame a bright reference: keep its blacks
        const float bot = r[kTone / 100], k = std::clamp((mr2 - bot) / std::max(1e-3f, mr - bot), 0.3f, 1.0f);
        for (float& v : r) v = bot + (v - bot) * k;
    }
    for (int i = 1; i < kTone; ++i) r[i] = std::max(r[i], r[i - 1]);
    const Curve look = quantileMap(canon, r, 0.35f, 2.5f, 3);  // canonical -> reference

    // target per quantile: canonical (or the source's own shape, by `contrast`), exposure kept by
    // (1 - `exposure`), then the look on top by `lookStrength`
    const float keep = (1 - ce) * key0;
    std::vector<float> t(kTone);
    for (int i = 0; i < kTone; ++i) {
        float q = (s0[i] - ce * key0) + (canon[i] + keep - (s0[i] - ce * key0)) * cc;
        t[i] = q + (look(q - keep) + keep - q) * ls;
    }
    const Curve g2 = quantileMap(s1, t, 0.33f, 2.5f, 3);

    auto canonicalDisplay = [linear, byLuminance, g1, g2](Vec3 code) {
        Vec3 lc = byLuminance(g2, byLuminance(g1, linear(code)));
        // highlights: roll luminance off toward white, and bring an out-of-range colour back toward its
        // luminance rather than clipping one channel
        const float y = std::max(luma709(lc), kStopFloor), y2 = softClip(y);
        lc = lc * (y2 / y);
        const float mx = std::max({lc.r, lc.g, lc.b});
        if (mx > 1.0f) lc = Vec3(y2, y2, y2) + (lc - Vec3(y2, y2, y2)) * ((1.0f - y2) / std::max(1e-6f, mx - y2));
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
            const float q = d[i] / dm, w = 1.0f / (1.0f + q * q * q * q), u = c.r - 0.5f;
            w0 += w; w1 += w * u; w2 += w * u * u; sa += w * c.g; sau += w * c.g * u; sb += w * c.b; sbu += w * c.b * u;
        }
        const double ridge = w2 + 0.05 * w0, det = w0 * ridge - w1 * w1;  // ridge on the slope
        if (w0 < 50 || std::abs(det) < 1e-12) break;
        offA[0] = float((ridge * sa - w1 * sau) / det); offA[1] = float((w0 * sau - w1 * sa) / det);
        offB[0] = float((ridge * sb - w1 * sbu) / det); offB[1] = float((w0 * sbu - w1 * sb) / det);
    }
    // only a residual cast, and only as far as the frame has neutrals to judge it by
    for (float* off : {offA, offB}) {
        off[0] = std::clamp(off[0], -0.03f, 0.03f) * cw * wbConfidence;
        off[1] = std::clamp(off[1], -0.06f, 0.06f) * cw * wbConfidence;
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
    const float satScale = 1 + (std::clamp(kCanonChroma / std::max(srcChroma, 1e-4f), 0.5f, 2.0f) - 1) * cc;
    auto canonical = [canonicalDisplay, offA, offB, satScale](Vec3 code) {
        Vec3 lab = rgbToOklab(canonicalDisplay(code));
        const float u = lab.r - 0.5f;
        lab.g = (lab.g - offA[0] - offA[1] * u) * satScale;
        lab.b = (lab.b - offB[0] - offB[1] * u) * satScale;
        return rgbToOklab(oklabToRgb(lab));  // in gamut
    };
    std::vector<Vec3> canonRgb(n);
    for (size_t i = 0; i < n; ++i) canonRgb[i] = oklabToRgb(canonical(src.px[i]));

    // ---- 5. the reference's look on the canonical frame (tone already done) ----
    MatchOptions mo = o.look;
    mo.matchTone = false;
    std::vector<Vec3> refRgb(ref.px.begin(), ref.px.end());
    for (Vec3& v : refRgb) v = clamp01(v);
    auto model = std::make_shared<Model>(refRgb, canonRgb, mo);
    auto full = [canonical, model, ls](Vec3 code) {
        Vec3 lab = canonical(code);
        return oklabToRgb(lerp(lab, model->applyLab(lab), ls));
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
    res.report = buf + model->report();
    return res;
}

}  // namespace lm
