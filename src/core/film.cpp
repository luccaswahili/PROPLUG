#include "film.h"

namespace lm {
namespace {

// Stock characteristics are original approximations of familiar film families, tuned by eye.
const NegativeStock kNegatives[] = {
    // name                         gamma R/G/B          speed offset (stops)   coupling grain size  halation
    {"Tungsten 500T",               {0.56f, 0.60f, 0.64f}, {0.00f, 0.00f, 0.00f},  0.10f, 0.55f, 1.20f, 0.60f},
    {"Daylight 250D",               {0.60f, 0.62f, 0.63f}, {0.00f, 0.00f, 0.00f},  0.15f, 0.35f, 1.00f, 0.40f},
    {"Fine Grain 50D",              {0.66f, 0.66f, 0.68f}, {0.00f, 0.00f, 0.00f},  0.25f, 0.15f, 0.80f, 0.30f},
    {"Portrait 400",                {0.52f, 0.56f, 0.55f}, {0.08f, 0.00f, -0.05f}, 0.00f, 0.40f, 1.00f, 0.35f},
    {"Low-Con Neutral",             {0.48f, 0.50f, 0.50f}, {0.00f, 0.00f, 0.00f}, -0.15f, 0.30f, 1.00f, 0.30f},
    {"Night 800T (heavy halation)", {0.56f, 0.60f, 0.64f}, {0.00f, 0.00f, 0.00f},  0.10f, 0.65f, 1.30f, 1.00f},
    {"Black & White 400",           {0.62f, 0.62f, 0.62f}, {0.00f, 0.00f, 0.00f}, -1.00f, 0.60f, 1.10f, 0.00f},
};

const PrintStock kPrints[] = {
    // name                       gamma R/G/B           dmax R/G/B            crosstalk
    {"None (scanned negative)",  {1.0f, 1.0f, 1.0f},   {3.0f, 3.0f, 3.0f},   0.0f},
    {"Cine Print Warm",          {3.20f, 3.10f, 3.35f}, {3.6f, 3.8f, 3.9f},  1.0f},
    {"Cine Print Cool",          {3.05f, 3.10f, 2.95f}, {3.7f, 3.7f, 3.6f},  0.7f},
    {"Low-Con Print",            {2.50f, 2.50f, 2.50f}, {3.0f, 3.0f, 3.0f},  0.6f},
    {"High-Con Print",           {3.80f, 3.80f, 3.80f}, {4.0f, 4.0f, 4.0f},  1.0f},
};

constexpr float kNegDmin = 0.20f, kNegRange = 2.40f;
constexpr float kPrintDmin = 0.06f;
constexpr float kGreyPrintDensity = 1.0f;
constexpr float kLog10_2 = 0.30103f;

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// unwanted absorptions of real dyes: cyan absorbs some green, magenta some blue and red,
// yellow a little green
Vec3 crosstalk(Vec3 d, float x) {
    return {d.r + 0.05f * x * d.g, d.g + 0.10f * x * d.r + 0.03f * x * d.b, d.b + 0.15f * x * d.g};
}

}  // namespace

int negativeStockCount() { return int(sizeof kNegatives / sizeof kNegatives[0]); }
const NegativeStock& negativeStock(int i) { return kNegatives[std::clamp(i, 0, negativeStockCount() - 1)]; }
int printStockCount() { return int(sizeof kPrints / sizeof kPrints[0]); }
const PrintStock& printStock(int i) { return kPrints[std::clamp(i, 0, printStockCount() - 1)]; }

FilmModel::FilmModel(const FilmSettings& s)
    : neg_(negativeStock(s.negative)), print_(printStock(s.print)), scanOnly_(s.print == 0) {
    coupling_ = (1.0f + neg_.coupling) * std::max(0.0f, s.saturation) - 1.0f;
    // grey reference ignores speed offsets so those show up as a deliberate cast
    for (int c = 0; c < 3; ++c) {
        float k = neg_.gamma[c] * 4.0f / kNegRange;
        greyNegDensity_[c] = kNegDmin + kNegRange * sigmoid(0.0f * k);
    }
    // printer lights: solve for the light that prints negative grey at the target density
    for (int c = 0; c < 3; ++c) {
        float range = print_.dmax[c] - kPrintDmin;
        float k = print_.gamma[c] * 4.0f / range;
        float t = (kGreyPrintDensity - kPrintDmin) / range;
        float x = std::log(t / (1.0f - t)) / k;  // print exposure that gives the target density
        printerLight_[c] = x + greyNegDensity_[c] + s.printerPoints[c] * 0.025f;
    }
    Vec3 g(kGreyPrintDensity, kGreyPrintDensity, kGreyPrintDensity);
    crosstalkGrey_ = crosstalk(g, print_.dyeCrosstalk) - g;
}

Vec3 FilmModel::negativeDensity(Vec3 lin) const {
    Vec3 E;
    for (int c = 0; c < 3; ++c) E[c] = std::log10(std::max(lin[c], 1e-5f) / 0.18f) + neg_.speedOffset[c] * kLog10_2;
    float m = (E.r + E.g + E.b) / 3.0f;
    Vec3 D;
    for (int c = 0; c < 3; ++c) {
        float e = E[c] + coupling_ * (E[c] - m);  // interlayer effects in log exposure
        float k = neg_.gamma[c] * 4.0f / kNegRange;
        D[c] = kNegDmin + kNegRange * sigmoid(k * e);
    }
    return D;
}

Vec3 FilmModel::printTransmittance(Vec3 dn) const {
    Vec3 dp;
    for (int c = 0; c < 3; ++c) {
        float range = print_.dmax[c] - kPrintDmin;
        float k = print_.gamma[c] * 4.0f / range;
        dp[c] = kPrintDmin + range * sigmoid(k * (printerLight_[c] - dn[c]));
    }
    dp = crosstalk(dp, print_.dyeCrosstalk) - crosstalkGrey_;
    Vec3 t;
    for (int c = 0; c < 3; ++c) t[c] = std::pow(10.0f, -(dp[c] - kPrintDmin));  // relative to clear base
    return t;
}

Vec3 FilmModel::apply(Vec3 lin) const {
    Vec3 dn = negativeDensity(lin);
    if (scanOnly_) {  // flat, log-like scan of the negative
        Vec3 v;
        for (int c = 0; c < 3; ++c) v[c] = clamp01(0.42f + (dn[c] - greyNegDensity_[c]) * 0.42f);
        return v;
    }
    Vec3 t = printTransmittance(dn);
    // projected print is display light: encode for a 2.4 gamma display
    return {std::pow(clamp01(t.r), 1 / 2.4f), std::pow(clamp01(t.g), 1 / 2.4f), std::pow(clamp01(t.b), 1 / 2.4f)};
}

}  // namespace lm
