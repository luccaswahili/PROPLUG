#include "scene.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace lm {
namespace {

// ---------- curves ----------
// Published camera log encodings (code values normalised 0..1, as Premiere hands them to effects).

inline float logC4Decode(float t) {
    const float a = (262144.0f - 16.0f) / 117.45f, b = (1023.0f - 95.0f) / 1023.0f, c = 95.0f / 1023.0f;
    const float s = (7.0f * std::log(2.0f) * std::exp2(7.0f - 14.0f * c / b)) / (a * b);
    const float tt = (std::exp2(14.0f * (-c / b) + 6.0f) - 64.0f) / a;
    return t >= 0 ? (std::exp2(14.0f * (t - c) / b + 6.0f) - 64.0f) / a : t * s + tt;
}
inline float logC4Encode(float x) {
    const float a = (262144.0f - 16.0f) / 117.45f, b = (1023.0f - 95.0f) / 1023.0f, c = 95.0f / 1023.0f;
    const float s = (7.0f * std::log(2.0f) * std::exp2(7.0f - 14.0f * c / b)) / (a * b);
    const float tt = (std::exp2(14.0f * (-c / b) + 6.0f) - 64.0f) / a;
    return x >= tt ? (std::log2(a * x + 64.0f) - 6.0f) / 14.0f * b + c : (x - tt) / s;
}
// Canon Log 3 (v1.2), scene reflectance with 18% grey at 34.3%
inline float cLog3Decode(float t) {
    float x = t < 0.097465473f ? -(std::pow(10.0f, (0.12783901f - t) / 0.36726845f) - 1.0f) / 14.98325f
              : t <= 0.15277891f ? (t - 0.12512219f) / 1.9754798f
                                 : (std::pow(10.0f, (t - 0.12240537f) / 0.36726845f) - 1.0f) / 14.98325f;
    return x * 0.9f;
}
inline float cLog3Encode(float x) {
    x /= 0.9f;
    return x < -0.014f ? -0.36726845f * std::log10(1.0f - 14.98325f * x) + 0.12783901f
           : x <= 0.014f ? 1.9754798f * x + 0.12512219f
                         : 0.36726845f * std::log10(14.98325f * x + 1.0f) + 0.12240537f;
}
struct FujiLog { float a, b, c, d, e, f, cut1, cut2; };
const FujiLog kFLog{0.555556f, 0.009468f, 0.344676f, 0.790453f, 8.735631f, 0.092864f, 0.00089f, 0.100537775223865f};
const FujiLog kFLog2{5.555556f, 0.064829f, 0.245281f, 0.384316f, 8.799461f, 0.092864f, 0.000889f, 0.100686685370811f};
inline float fLogDecode(const FujiLog& k, float t) { return t >= k.cut2 ? (std::pow(10.0f, (t - k.d) / k.c) - k.b) / k.a : (t - k.f) / k.e; }
inline float fLogEncode(const FujiLog& k, float x) { return x >= k.cut1 ? k.c * std::log10(k.a * x + k.b) + k.d : k.e * x + k.f; }
// Apple Log
constexpr float kAlR0 = -0.05641088f, kAlRt = 0.01f, kAlC = 47.28711236f, kAlB = 0.00964052f, kAlG = 0.08550479f,
                kAlBeta = 0.69336945f;
inline float appleLogEncode(float x) {
    if (x >= kAlRt) return kAlG * std::log2(x + kAlB) + kAlBeta;
    if (x >= kAlR0) return kAlC * (x - kAlR0) * (x - kAlR0);
    return 0;
}
inline float appleLogDecode(float t) {
    const float pt = kAlC * (kAlRt - kAlR0) * (kAlRt - kAlR0);
    if (t >= pt) return std::exp2((t - kAlBeta) / kAlG) - kAlB;
    return std::sqrt(std::max(t, 0.0f) / kAlC) + kAlR0;
}
// Blackmagic Film Generation 5
constexpr float kBmA = 0.08692876065491224f, kBmB = 0.005494072432257808f, kBmC = 0.5300133392291939f,
                kBmD = 8.283605932402494f, kBmE = 0.09246575342465753f, kBmLinCut = 0.005f;
inline float bmdDecode(float t) { return t < kBmD * kBmLinCut + kBmE ? (t - kBmE) / kBmD : std::exp((t - kBmC) / kBmA) - kBmB; }
inline float bmdEncode(float x) { return x < kBmLinCut ? kBmD * x + kBmE : kBmA * std::log(x + kBmB) + kBmC; }
// RED Log3G10
inline float log3G10Decode(float t) { return t < 0 ? t / 15.1927f - 0.01f : (std::pow(10.0f, t / 0.224282f) - 1.0f) / 155.975327f - 0.01f; }
inline float log3G10Encode(float x) { x += 0.01f; return x < 0 ? x * 15.1927f : 0.224282f * std::log10(x * 155.975327f + 1.0f); }
// DJI D-Log
inline float dLogDecode(float t) { return t <= 0.14f ? (t - 0.0929f) / 6.025f : (std::pow(10.0f, 3.89616f * t - 2.27752f) - 0.0108f) / 0.9892f; }
inline float dLogEncode(float x) { return x <= 0.0078f ? 6.025f * x + 0.0929f : std::log10(x * 0.9892f + 0.0108f) * 0.256663f + 0.584555f; }
// DaVinci Intermediate (Blackmagic, DaVinci Wide Gamut Intermediate): Resolve's own log, which footage
// exported through Resolve (stock libraries' "LOG" clips) often carries
inline float diDecode(float t) { return t <= 0.02740668f ? t / 10.44426855f : std::exp2(t / 0.07329248f - 7.0f) - 0.0075f; }
inline float diEncode(float x) { return x <= 0.00262409f ? x * 10.44426855f : (std::log2(x + 0.0075f) + 7.0f) * 0.07329248f; }

// ---------- gamuts ----------

struct Primaries { float rx, ry, gx, gy, bx, by; };
const Primaries kRec709{0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f};
const Primaries kRec2020{0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f};
const Primaries kSGamut3Cine{0.766f, 0.275f, 0.225f, 0.800f, 0.089f, -0.087f};
const Primaries kAWG3{0.6840f, 0.3130f, 0.2210f, 0.8480f, 0.0861f, -0.1020f};
const Primaries kAWG4{0.7347f, 0.2653f, 0.1424f, 0.8576f, 0.0991f, -0.0308f};
const Primaries kCinemaGamut{0.74f, 0.27f, 0.17f, 1.14f, 0.08f, -0.10f};
const Primaries kVGamut{0.730f, 0.280f, 0.165f, 0.840f, 0.100f, -0.030f};
const Primaries kBMDWideGen5{0.7177f, 0.3171f, 0.2280f, 0.8616f, 0.1006f, -0.0820f};
const Primaries kRedWide{0.780308f, 0.304253f, 0.121595f, 1.493994f, 0.095612f, -0.084589f};
const Primaries kDGamut{0.71f, 0.31f, 0.21f, 0.88f, 0.09f, -0.08f};
const Primaries kDaVinciWide{0.8000f, 0.3130f, 0.1682f, 0.9877f, 0.0790f, -0.1155f};

Mat3 rgbToXyzFor(const Primaries& p) {  // normalised primary matrix, D65 white
    auto col = [](float x, float y) { return Vec3(x / y, 1.0f, (1 - x - y) / y); };
    Vec3 R = col(p.rx, p.ry), G = col(p.gx, p.gy), B = col(p.bx, p.by), W = col(kD65x, kD65y);
    Mat3 P{{{R.r, G.r, B.r}, {R.g, G.g, B.g}, {R.b, G.b, B.b}}};
    Vec3 S = inverse(P) * W;
    return Mat3{{{R.r * S.r, G.r * S.g, B.r * S.b}, {R.g * S.r, G.g * S.g, B.g * S.b}, {R.b * S.r, G.b * S.g, B.b * S.b}}};
}

const Primaries& primariesOf(Transfer t) {
    switch (t) {
        case Transfer::SLog3: return kSGamut3Cine;
        case Transfer::LogC3: return kAWG3;
        case Transfer::LogC4: return kAWG4;
        case Transfer::CLog3: return kCinemaGamut;
        case Transfer::VLog: return kVGamut;
        case Transfer::FLog: case Transfer::FLog2: case Transfer::AppleLog: return kRec2020;
        case Transfer::BMDFilm5: return kBMDWideGen5;
        case Transfer::Log3G10: return kRedWide;
        case Transfer::DLog: return kDGamut;
        case Transfer::DaVinciInt: return kDaVinciWide;
        default: return kRec709;
    }
}

struct GamutTable {
    Mat3 to709[kTransferCount], from709[kTransferCount];
    GamutTable() {
        Mat3 xyzTo709 = inverse(rgbToXyzFor(kRec709));
        for (int i = 0; i < kTransferCount; ++i) {
            to709[i] = xyzTo709 * rgbToXyzFor(primariesOf(Transfer(i)));
            from709[i] = inverse(to709[i]);
        }
    }
};
const GamutTable& gamuts() { static const GamutTable g; return g; }

// ---------- CCT ----------

// Planckian locus in CIE 1960 uv (Krystek's approximation, 1000..15000 K, extrapolated a little).
void planckUV(double T, double& u, double& v) {
    u = (0.860117757 + 1.54118254e-4 * T + 1.28641212e-7 * T * T) / (1 + 8.42420235e-4 * T + 7.08145163e-7 * T * T);
    v = (0.317398726 + 4.22806245e-5 * T + 4.20481691e-8 * T * T) / (1 - 2.89741816e-5 * T + 1.61456053e-7 * T * T);
}
void planckNormal(double T, double& nu, double& nv) {  // unit normal, pointing to +v (green)
    double u0, v0, u1, v1;
    planckUV(T * 0.999, u0, v0);
    planckUV(T * 1.001, u1, v1);
    double tu = u1 - u0, tv = v1 - v0, n = std::hypot(tu, tv);
    nu = -tv / n; nv = tu / n;
    if (nv < 0) { nu = -nu; nv = -nv; }
}
void xyToUV(double x, double y, double& u, double& v) {
    double d = -2 * x + 12 * y + 3;
    u = 4 * x / d; v = 6 * y / d;
}
// CCT and signed Duv (positive = green, above the locus)
void cctDuv(double x, double y, double& T, double& duv) {
    double u, v;
    xyToUV(x, y, u, v);
    auto dist = [&](double mired) { double pu, pv; planckUV(1e6 / mired, pu, pv); return std::hypot(u - pu, v - pv); };
    double lo = 1e6 / 30000, hi = 1e6 / 1200;  // golden section over mireds
    const double g = 0.6180339887;
    double a = hi - g * (hi - lo), b = lo + g * (hi - lo), fa = dist(a), fb = dist(b);
    for (int i = 0; i < 80; ++i) {
        if (fa < fb) { hi = b; b = a; fb = fa; a = hi - g * (hi - lo); fa = dist(a); }
        else { lo = a; a = b; fa = fb; b = lo + g * (hi - lo); fb = dist(b); }
    }
    T = 1e6 / (0.5 * (lo + hi));
    double pu, pv, nu, nv;
    planckUV(T, pu, pv);
    planckNormal(T, nu, nv);
    duv = (u - pu) * nu + (v - pv) * nv;
}
double d65Duv() {
    static const double d = [] { double T, duv; cctDuv(kD65x, kD65y, T, duv); return duv; }();
    return d;
}

// ---------- statistics ----------

float quantile(std::vector<float> v, float q) {
    if (v.empty()) return 0;
    size_t k = std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}
std::vector<float> quantiles(std::vector<float> v, int n) {
    std::vector<float> q(n, 0.0f);
    if (v.empty()) return q;
    std::sort(v.begin(), v.end());
    for (int i = 0; i < n; ++i) {
        double pos = (double(i) / (n - 1)) * (v.size() - 1);
        size_t k = size_t(pos);
        double f = pos - k;
        q[i] = float(v[k] + (k + 1 < v.size() ? f * (v[k + 1] - v[k]) : 0.0));
    }
    return q;
}
inline float smoothstep(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}

}  // namespace

// ---------- public: curves ----------

const char* transferName(Transfer t) {
    static const char* n[] = {"Rec.709", "sRGB", "Sony S-Log3 / S-Gamut3.Cine", "ARRI LogC3 / AWG3", "ARRI LogC4 / AWG4",
                              "Canon Log 3 / Cinema Gamut", "Panasonic V-Log / V-Gamut", "Fujifilm F-Log / F-Gamut",
                              "Fujifilm F-Log2 / F-Gamut", "Apple Log / Rec.2020", "Blackmagic Film Gen5 / Wide Gamut",
                              "RED Log3G10 / REDWideGamutRGB", "DJI D-Log / D-Gamut",
                              "DaVinci Intermediate / Wide Gamut"};
    return n[std::clamp(int(t), 0, kTransferCount - 1)];
}
const char* transferShortName(Transfer t) {
    static const char* n[] = {"Rec.709", "sRGB", "S-Log3", "LogC3", "LogC4", "Canon Log 3", "V-Log", "F-Log", "F-Log2",
                              "Apple Log", "BMD Film Gen5", "Log3G10", "D-Log", "DaVinci Int."};
    return n[std::clamp(int(t), 0, kTransferCount - 1)];
}

float transferDecode(Transfer t, float v) {
    switch (t) {
        case Transfer::SRGB: return v <= 0 ? v / 12.92f : srgbToLinear(v);
        case Transfer::SLog3: return sLog3Decode(v);
        case Transfer::LogC3: return logC3Decode(v);
        case Transfer::LogC4: return logC4Decode(v);
        case Transfer::CLog3: return cLog3Decode(v);
        case Transfer::VLog: return vLogDecode(v);
        case Transfer::FLog: return fLogDecode(kFLog, v);
        case Transfer::FLog2: return fLogDecode(kFLog2, v);
        case Transfer::AppleLog: return appleLogDecode(v);
        case Transfer::BMDFilm5: return bmdDecode(v);
        case Transfer::Log3G10: return log3G10Decode(v);
        case Transfer::DLog: return dLogDecode(v);
        case Transfer::DaVinciInt: return diDecode(v);
        default: return v <= 0 ? v / 4.5f : rec709Decode(v);
    }
}
float transferEncode(Transfer t, float x) {
    switch (t) {
        case Transfer::SRGB: return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
        case Transfer::SLog3: return sLog3Encode(x);
        case Transfer::LogC3: return logC3Encode(x);
        case Transfer::LogC4: return logC4Encode(x);
        case Transfer::CLog3: return cLog3Encode(x);
        case Transfer::VLog: return vLogEncode(x);
        case Transfer::FLog: return fLogEncode(kFLog, x);
        case Transfer::FLog2: return fLogEncode(kFLog2, x);
        case Transfer::AppleLog: return appleLogEncode(x);
        case Transfer::BMDFilm5: return bmdEncode(x);
        case Transfer::Log3G10: return log3G10Encode(x);
        case Transfer::DLog: return dLogEncode(x);
        case Transfer::DaVinciInt: return diEncode(x);
        default: return x <= 0 ? x * 4.5f : rec709Encode(x);
    }
}
Mat3 gamutToRec709(Transfer t) { return gamuts().to709[std::clamp(int(t), 0, kTransferCount - 1)]; }
Mat3 rec709ToGamut(Transfer t) { return gamuts().from709[std::clamp(int(t), 0, kTransferCount - 1)]; }
Vec3 decodeToRec709(Transfer t, Vec3 c, bool withGamut) {
    Vec3 l(transferDecode(t, c.r), transferDecode(t, c.g), transferDecode(t, c.b));
    return isLog(t) && withGamut ? gamuts().to709[int(t)] * l : l;
}
Vec3 encodeFromRec709(Transfer t, Vec3 l) {
    if (isLog(t)) l = gamuts().from709[int(t)] * l;
    return {transferEncode(t, l.r), transferEncode(t, l.g), transferEncode(t, l.b)};
}

int transferFromName(const std::string& text) {
    std::string s;
    for (char c : text) s += char(std::tolower((unsigned char)c));
    auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
    if (has("s-log3") || has("slog3") || has("s-log 3") || has("s-gamut3")) return int(Transfer::SLog3);
    // Artlist re-exports its "LOG" clips through Resolve, camera curve and tags lost; F-Log fits them best
    if (has("artlist") && has("log")) return int(Transfer::FLog);
    if (has("logc4") || has("log c4") || has("awg4")) return int(Transfer::LogC4);
    if (has("logc") || has("log c") || has("awg3") || has("alexa")) return int(Transfer::LogC3);
    if (has("canon log 3") || has("c-log3") || has("clog3") || has("canon log3")) return int(Transfer::CLog3);
    if (has("v-log") || has("vlog") || has("v-gamut")) return int(Transfer::VLog);
    if (has("f-log2") || has("flog2")) return int(Transfer::FLog2);
    if (has("f-log") || has("flog")) return int(Transfer::FLog);
    if (has("apple log") || has("applelog")) return int(Transfer::AppleLog);
    if (has("davinci intermediate") || has("davinci wide") || has("dwg")) return int(Transfer::DaVinciInt);
    if (has("blackmagic") || has("bmd film") || has("braw")) return int(Transfer::BMDFilm5);
    if (has("log3g10") || has("redwidegamut") || has("ipp2")) return int(Transfer::Log3G10);
    if (has("d-log") || has("dlog")) return int(Transfer::DLog);
    if (has("srgb")) return int(Transfer::SRGB);
    if (has("rec. 709") || has("rec.709") || has("rec709") || has("bt.709") || has("bt709")) return int(Transfer::Rec709);
    return -1;
}

// ---------- public: white points ----------

Mat3 inverse(const Mat3& a) {
    const auto& m = a.m;
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    float id = 1.0f / det;
    Mat3 r;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return r;
}
Mat3 operator*(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
Vec3 rec709ToXYZ(Vec3 c) { static const Mat3 M = rgbToXyzFor(kRec709); return M * c; }
Vec3 xyzToRec709(Vec3 c) { static const Mat3 M = inverse(rgbToXyzFor(kRec709)); return M * c; }

void xyToCctTint(float x, float y, float& cct, float& tint) {
    double T, duv;
    cctDuv(x, y, T, duv);
    cct = float(T);
    tint = float(-(duv - d65Duv()) * 1000.0);
}
void cctTintToXy(float cct, float tint, float& x, float& y) {
    double T = std::clamp(double(cct), 1200.0, 30000.0), duv = d65Duv() - tint / 1000.0, u, v, nu, nv;
    planckUV(T, u, v);
    planckNormal(T, nu, nv);
    u += duv * nu; v += duv * nv;
    double d = 2 * u - 8 * v + 4;
    x = float(3 * u / d); y = float(2 * v / d);
}

Mat3 adaptation(float fx, float fy, float tx, float ty) {
    static const Mat3 B{{{0.8951f, 0.2664f, -0.1614f}, {-0.7502f, 1.7135f, 0.0367f}, {0.0389f, -0.0685f, 1.0296f}}};
    static const Mat3 Bi = inverse(B), toXYZ = rgbToXyzFor(kRec709), fromXYZ = inverse(toXYZ);
    Vec3 f = B * Vec3(fx / fy, 1.0f, (1 - fx - fy) / fy), t = B * Vec3(tx / ty, 1.0f, (1 - tx - ty) / ty);
    Mat3 D{{{t.r / f.r, 0, 0}, {0, t.g / f.g, 0}, {0, 0, t.b / f.b}}};
    return fromXYZ * (Bi * (D * (B * toXYZ)));
}

// ---------- public: analysis ----------

Image downsample(const Image& in, int maxWidth) {
    if (in.width <= maxWidth) return in;
    Image o;
    o.width = maxWidth;
    o.height = std::max(1, int(std::lround(double(in.height) * maxWidth / in.width)));
    o.px.resize(size_t(o.width) * o.height);
    for (int y = 0; y < o.height; ++y) {
        int y0 = y * in.height / o.height, y1 = std::max(y0 + 1, (y + 1) * in.height / o.height);
        for (int x = 0; x < o.width; ++x) {
            int x0 = x * in.width / o.width, x1 = std::max(x0 + 1, (x + 1) * in.width / o.width);
            Vec3 acc;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) acc = acc + in.at(xx, yy);
            o.px[size_t(y) * o.width + x] = acc * (1.0f / float((y1 - y0) * (x1 - x0)));
        }
    }
    return o;
}

TransferGuess detectTransfer(const Image& img) {
    TransferGuess g;
    std::vector<float> lo, hi, luma, chroma;
    lo.reserve(img.px.size()); hi.reserve(img.px.size()); luma.reserve(img.px.size());
    for (const Vec3& c : img.px) {
        float mn = std::min({c.r, c.g, c.b}), mx = std::max({c.r, c.g, c.b});
        lo.push_back(mn); hi.push_back(mx); luma.push_back(luma709(c));
        Vec3 lab = rgbToOklab(clamp01(c));
        if (lab.r > 0.25f && lab.r < 0.85f) chroma.push_back(std::hypot(lab.g, lab.b));
    }
    if (lo.empty()) return g;
    // the top is judged at the 99th percentile: a sun or speculars reaching the top codes don't make a
    // log frame display-referred (stock "LOG" exports often let them clip)
    const float floor = quantile(lo, 0.005f), top = quantile(hi, 0.99f);
    const float spread = quantile(luma, 0.95f) - quantile(luma, 0.05f);
    const float sat = chroma.empty() ? 0.0f : quantile(chroma, 0.5f);
    // each cue: 0 = display-like .. 1 = log-like
    const float cFloor = smoothstep(0.05f, 0.09f, floor);     // log blacks sit at 0.09-0.13 and above
    const float cTop = 1 - smoothstep(0.70f, 0.88f, top);     // log rarely reaches the top codes
    const float cSpread = 1 - smoothstep(0.38f, 0.50f, spread);
    const float cSat = 1 - smoothstep(0.02f, 0.04f, sat);
    // Log needs a raised floor AND the rest: a faded grade lifts blacks too, but keeps contrast,
    // highlights or colour. A frame that reaches the top codes or true black is display-referred.
    g.logScore = cFloor * (0.4f * cSpread + 0.35f * cTop + 0.25f * cSat);
    if (top >= 0.93f || floor < 0.045f) g.logScore = std::min(g.logScore, 0.2f);
    char buf[200];
    std::snprintf(buf, sizeof buf, "floor %.3f top %.3f spread %.3f chroma %.3f", floor, top, spread, sat);
    g.why = buf;
    if (g.logScore < 0.55f) return g;
    // Which log can't be told from a picture: the curves differ by about an exposure and a contrast
    // change. S-Log3 sits in the middle of the family and stands in unless metadata, a file name (an
    // Artlist "LOG" clip reads as F-Log) or the user says otherwise.
    g.transfer = kGuessedLog;
    return g;
}

std::string SceneAnalysis::json() const {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"transfer\":\"%s\",\"transferIndex\":%d,\"log\":%s,\"logScore\":%.2f,\"fromUser\":%s,"
                  "\"exposure\":%.2f,\"cct\":%.0f,\"tint\":%.1f,\"contrast\":%.2f,\"saturation\":%.3f,"
                  "\"blackClip\":%.3f,\"whiteClip\":%.3f,\"neutralShare\":%.3f}",
                  transferShortName(transfer), int(transfer), isLog(transfer) ? "true" : "false", logScore,
                  transferFromUser ? "true" : "false", exposure, cct, tint, contrast, saturation, blackClip, whiteClip,
                  neutralShare);
    return buf;
}


Vec3 estimateIlluminant(const Image& img, const std::vector<Vec3>& lin, const std::vector<uint8_t>& valid,
                        float& neutralShare, const std::vector<Vec3>* measure) {
    const int w = img.width, h = img.height;
    const size_t n = lin.size();
    const std::vector<Vec3>& M = measure ? *measure : lin;  // pixels are chosen in `lin`, their colour read in M
    auto norm = [](Vec3 c) { float y = luma709(c); return y > 1e-9f ? c * (1.0f / y) : Vec3(1, 1, 1); };
    auto logGeoMean = [&](const std::vector<std::pair<Vec3, float>>& v, Vec3 fallback) {
        double a[3] = {0, 0, 0}, sw = 0;
        for (auto& [c, wt] : v) {
            if (wt <= 0 || c.r <= 0 || c.g <= 0 || c.b <= 0) continue;
            for (int k = 0; k < 3; ++k) a[k] += wt * std::log(c[k]);
            sw += wt;
        }
        if (sw <= 0) return fallback;
        return norm(Vec3(float(std::exp(a[0] / sw)), float(std::exp(a[1] / sw)), float(std::exp(a[2] / sw))));
    };
    auto geo2 = [&](Vec3 a, Vec3 b) { return norm(Vec3(std::sqrt(a.r * b.r), std::sqrt(a.g * b.g), std::sqrt(a.b * b.b))); };

    std::vector<float> ys;
    for (size_t i = 0; i < n; ++i) if (valid[i]) ys.push_back(luma709(lin[i]));
    if (ys.size() < 50) { neutralShare = 0; return {1, 1, 1}; }
    const float y10 = quantile(ys, 0.10f), y97 = quantile(ys, 0.97f), y995 = quantile(ys, 0.995f);

    // grey-edge (first order): mean absolute gradient per channel
    auto greyEdge = [&](const std::vector<Vec3>& L) {
        double e[3] = {0, 0, 0};
        int cnt = 0;
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                size_t i = size_t(y) * w + x;
                if (!valid[i] || !valid[i - 1] || !valid[i + 1] || !valid[i - w] || !valid[i + w]) continue;
                for (int k = 0; k < 3; ++k) {
                    float gx = L[i + 1][k] - L[i - 1][k], gy = L[i + w][k] - L[i - w][k];
                    e[k] += std::sqrt(gx * gx + gy * gy);
                }
                ++cnt;
            }
        return cnt > 20 && e[0] > 0 && e[1] > 0 && e[2] > 0 ? norm(Vec3(float(e[0]), float(e[1]), float(e[2]))) : Vec3(1, 1, 1);
    };
    // white patch: the brightest unclipped pixels
    std::vector<std::pair<Vec3, float>> bright, brightM;
    for (size_t i = 0; i < n; ++i) {
        if (!valid[i]) continue;
        float y = luma709(lin[i]);
        if (y >= y97 && y <= y995) { bright.push_back({norm(lin[i]), 1.0f}); brightM.push_back({norm(M[i]), 1.0f}); }
    }
    const Vec3 edge = greyEdge(lin), edgeM = measure ? greyEdge(M) : edge;
    const Vec3 patch = logGeoMean(bright, edge), patchM = logGeoMean(brightM, edgeM);

    // Near-neutral pixels, iterated from the edge/patch estimate. "Near" is by rank (the most neutral
    // fifth of the frame) rather than a fixed distance: a grade's contrast or saturation spreads every
    // pixel's colour, so a fixed distance would pick a different set, but it hardly reorders them.
    Vec3 est = geo2(edge, patch);
    std::vector<std::pair<size_t, float>> neutral;
    double share = 0;
    std::vector<size_t> cand;
    for (size_t i = 0; i < n; ++i)
        if (valid[i] && luma709(lin[i]) >= y10 && luma709(lin[i]) <= y995) cand.push_back(i);
    if (cand.size() >= 50) {
        std::vector<float> dist(cand.size());
        for (int it = 0; it < 6; ++it) {
            for (size_t k = 0; k < cand.size(); ++k) {
                Vec3 c = norm(lin[cand[k]]);
                Vec3 cc(c.r / est.r, c.g / est.g, c.b / est.b);
                float sum = cc.r + cc.g + cc.b;
                dist[k] = std::hypot(cc.r / sum - 1.0f / 3, cc.g / sum - 1.0f / 3);
            }
            const float d20 = std::max(quantile(dist, 0.2f), 1e-4f);
            neutral.clear();
            for (size_t k = 0; k < cand.size(); ++k) {
                float t = dist[k] / d20;
                if (t <= 1.5f) neutral.push_back({cand[k], std::exp(-t * t)});
            }
            // how much of the frame is close to neutral in absolute terms
            share = d20 < 0.03f ? 0.2 : d20 < 0.06f ? 0.2 * (0.06 - d20) / 0.03 : 0.0;
            std::vector<std::pair<Vec3, float>> v;
            for (auto& [i, wt] : neutral) v.push_back({norm(lin[i]), wt});
            Vec3 next = logGeoMean(v, est);
            bool done = std::abs(next.r - est.r) + std::abs(next.b - est.b) < 1e-4f;
            est = next;
            if (done) break;
        }
    }
    neutralShare = float(share);
    Vec3 estM = est;
    if (measure) {
        std::vector<std::pair<Vec3, float>> v;
        for (auto& [i, wt] : neutral) v.push_back({norm(M[i]), wt});
        estM = logGeoMean(v, geo2(edgeM, patchM));
    }
    // the neutral set, with a little of the white patch
    float wn = neutral.empty() ? 0.0f : 0.85f;
    Vec3 alt = patchM;
    Vec3 out;
    for (int k = 0; k < 3; ++k) out[k] = std::exp(wn * std::log(estM[k]) + (1 - wn) * std::log(alt[k]));
    return norm(out);
}


SceneAnalysis analyseScene(const Image& in, const AnalysisOptions& opt) {
    SceneAnalysis a;
    const Image img = downsample(in, 480);
    if (opt.transfer >= 0 && opt.transfer < kTransferCount) {
        a.transfer = Transfer(opt.transfer);
        a.transferFromUser = true;
        a.logScore = isLog(a.transfer) ? 1.0f : 0.0f;
    } else {
        TransferGuess g = detectTransfer(img);
        a.transfer = g.transfer;
        a.logScore = g.logScore;
    }
    const size_t n = img.px.size();
    if (!n) return a;
    std::vector<Vec3> lin(n);
    std::vector<uint8_t> valid(n, 1);
    size_t blacks = 0, whites = 0;
    // Clipping, judged in camera linear relative to the frame's own ceiling, so the same scene gives the
    // same mask whatever curve it was encoded with or however it was exposed.
    std::vector<float> peak(n);
    for (size_t i = 0; i < n; ++i) {
        lin[i] = decodeToRec709(a.transfer, img.px[i]);
        const Vec3& c = img.px[i];
        peak[i] = std::max({transferDecode(a.transfer, c.r), transferDecode(a.transfer, c.g), transferDecode(a.transfer, c.b)});
    }
    const float top = quantile(peak, 0.999f);
    size_t nearTop = 0;
    for (float p : peak) nearTop += p >= top * 0.97f;
    const bool clips = nearTop >= n / 300;  // a pile-up at the top is a clip, a lone highlight is not
    for (size_t i = 0; i < n; ++i) {
        if (clips && peak[i] >= top * 0.97f) { valid[i] = 0; ++whites; }
        if (peak[i] <= top * (1.0f / 2048)) { valid[i] = 0; ++blacks; }
        if (luma709(lin[i]) <= 0) valid[i] = 0;
    }
    a.blackClip = float(blacks) / n;
    a.whiteClip = float(whites) / n;

    Vec3 ill(1, 1, 1);
    if (opt.estimateWhite) ill = estimateIlluminant(img, lin, valid, a.neutralShare);
    Vec3 xyz = rec709ToXYZ(ill);
    float sxyz = xyz.r + xyz.g + xyz.b;
    a.whiteX = xyz.r / sxyz; a.whiteY = xyz.g / sxyz;
    xyToCctTint(a.whiteX, a.whiteY, a.cct, a.tint);
    // keep the estimate in a plausible range (very coloured scenes with no neutrals)
    float cct = std::clamp(a.cct, 2000.0f, 12000.0f), tint = std::clamp(a.tint, -40.0f, 40.0f);
    if (cct != a.cct || tint != a.tint) {
        a.cct = cct; a.tint = tint;
        cctTintToXy(cct, tint, a.whiteX, a.whiteY);
    }
    const Mat3 wb = adaptation(a.whiteX, a.whiteY, kD65x, kD65y);

    std::vector<float> stops, chroma;
    stops.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Vec3 l = wb * lin[i];
        float y = std::max(luma709(l), 1.0f / 4096);
        float s = std::log2(y / 0.18f);
        stops.push_back(s);
    }
    a.stops = quantiles(stops, 257);
    {  // trimmed log-average of every pixel: leaving clipped ones out would bias the key down
        std::vector<float> k = stops;
        std::sort(k.begin(), k.end());
        size_t lo = k.size() / 20, hi = k.size() - k.size() / 20;
        double s = 0;
        for (size_t i = lo; i < hi; ++i) s += k[i];
        a.exposure = float(s / std::max<size_t>(1, hi - lo));
    }
    a.contrast = a.stops[256 * 95 / 100] - a.stops[256 * 5 / 100];
    const float g = std::exp2(-a.exposure);
    for (size_t i = 0; i < n; i += 2) {
        Vec3 l = wb * lin[i] * g;
        Vec3 d(rec709Encode(std::min(1.0f, std::max(0.0f, l.r))), rec709Encode(std::min(1.0f, std::max(0.0f, l.g))),
               rec709Encode(std::min(1.0f, std::max(0.0f, l.b))));
        Vec3 lab = rgbToOklab(d);
        if (lab.r > 0.2f && lab.r < 0.9f) chroma.push_back(std::hypot(lab.g, lab.b));
    }
    a.saturation = chroma.empty() ? 0.0f : float(std::accumulate(chroma.begin(), chroma.end(), 0.0) / chroma.size());
    return a;
}

std::vector<float> evenQuantiles(std::vector<float> v, int n) { return quantiles(std::move(v), n); }

Normaliser makeNormaliser(const SceneAnalysis& a, float whiteAmount, float exposureAmount) {
    Normaliser nm;
    nm.transfer = a.transfer;
    whiteAmount = clamp01(whiteAmount);
    float fx = kD65x + (a.whiteX - kD65x) * whiteAmount, fy = kD65y + (a.whiteY - kD65y) * whiteAmount;
    nm.white = adaptation(fx, fy, kD65x, kD65y);
    nm.gain = std::exp2(-a.exposure * clamp01(exposureAmount));
    return nm;
}

}  // namespace lm
