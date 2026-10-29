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

    // Re-integrates the tone curve with its slope clamped, then shifts it so the source's darkest
    // tones (1st percentile) still land where the unclamped curve put them. The minimum slope only
    // applies inside the source's tonal range; outside it the curve may stay flat.
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
        float shift = interp(lo, knots_, c) - interp(lo, knots_, out);
        for (float& v : out) v = clamp01(v + shift);
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

}  // namespace lm
