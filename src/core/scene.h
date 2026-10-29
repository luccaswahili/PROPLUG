// Source analysis: what a frame's code values mean (log or display, which curve), how it is exposed,
// what light it was balanced for and how contrasty it is. The matcher uses this to bring any clip to
// one canonical state before applying a reference's look, so a match depends on what is in the shot,
// not on how the clip was shot or graded before.
#pragma once
#include <string>
#include <vector>
#include "color.h"

namespace lm {

// ---------- transfer curves + gamuts ----------

enum class Transfer {
    Rec709 = 0, SRGB, SLog3, LogC3, LogC4, CLog3, VLog, FLog, FLog2, AppleLog, BMDFilm5, Log3G10, DLog, Count
};
constexpr int kTransferCount = int(Transfer::Count);
const char* transferName(Transfer t);       // "Sony S-Log3 / S-Gamut3.Cine"
const char* transferShortName(Transfer t);  // "S-Log3"
inline bool isLog(Transfer t) { return t != Transfer::Rec709 && t != Transfer::SRGB; }

float transferDecode(Transfer t, float code);   // code value 0..1 -> linear (camera primaries)
float transferEncode(Transfer t, float linear);  // and back
Mat3 gamutToRec709(Transfer t);                  // linear camera primaries -> linear Rec.709
Mat3 rec709ToGamut(Transfer t);
// code -> linear Rec.709 (may be negative out of gamut). Without the gamut, a log curve's camera primaries
// are taken as Rec.709: used when the curve was only guessed from the picture, as the gamut can't be.
Vec3 decodeToRec709(Transfer t, Vec3 code, bool withGamut = true);
Vec3 encodeFromRec709(Transfer t, Vec3 linear);

// Transfer named in clip metadata or a file name ("S-Log3", "LogC", "V-Log", ...), or -1.
int transferFromName(const std::string& text);

// ---------- white points ----------

Mat3 inverse(const Mat3& m);
Mat3 operator*(const Mat3& a, const Mat3& b);
Vec3 rec709ToXYZ(Vec3 rgb);
Vec3 xyzToRec709(Vec3 xyz);
// Correlated colour temperature (K) + tint of a chromaticity. Tint is the distance from the
// Planckian locus in CIE 1960 uv, x1000, positive = magenta, measured relative to D65's own offset
// so D65 reads 6504 K / 0.
void xyToCctTint(float x, float y, float& cct, float& tint);
void cctTintToXy(float cct, float tint, float& x, float& y);
// Bradford adaptation in linear Rec.709: a colour seen as white under `from` (xy) becomes white under `to`.
Mat3 adaptation(float fromX, float fromY, float toX, float toY);
constexpr float kD65x = 0.3127f, kD65y = 0.3290f;

// ---------- analysis ----------

struct Image {  // RGB code values, row major
    int width = 0, height = 0;
    std::vector<Vec3> px;
    const Vec3& at(int x, int y) const { return px[size_t(y) * width + x]; }
};
Image downsample(const Image& in, int maxWidth);

struct TransferGuess {
    Transfer transfer = Transfer::Rec709;
    float logScore = 0;  // 0 = clearly display-referred .. 1 = clearly log
    std::string why;
};
// Log or display, from the frame alone: raised black floor, highlights that stop well short of
// clipping, low saturation and a histogram bunched in the middle are log.
TransferGuess detectTransfer(const Image& codes);

struct SceneAnalysis {
    Transfer transfer = Transfer::Rec709;
    float logScore = 0;
    bool transferFromUser = false;  // set by an override / metadata rather than detected
    float exposure = 0;             // stops: scene key vs 18% grey (after white balance)
    float whiteX = kD65x, whiteY = kD65y;  // chromaticity neutral objects have in the frame
    float cct = 6504, tint = 0;
    float contrast = 0;    // stops between the 5th and 95th percentile of luminance
    float saturation = 0;  // mean OKLab chroma of the white-balanced, display-encoded frame
    float blackClip = 0, whiteClip = 0;  // fraction of pixels at the floor / clipped
    float neutralShare = 0;              // fraction of pixels used as near-neutral for white balance
    std::vector<float> stops;            // 257 quantiles of log2(luminance / 0.18), white balanced, not exposed
    std::string json() const;
};

struct AnalysisOptions {
    int transfer = -1;  // -1 = detect
    bool estimateWhite = true;
};
SceneAnalysis analyseScene(const Image& codes, const AnalysisOptions& opt = {});

// Illuminant (as linear Rec.709, luminance 1) of a linear image (img gives the size), from near-neutral
// pixels, a white-patch percentile and grey-edge, with invalid (clipped / black), very dark, skin-like
// and strongly coloured pixels left out or down-weighted. neutralShare: fraction of pixels used as neutral.
// With `measure`, pixels are chosen in `lin` but their colour is read from `measure` (same layout).
Vec3 estimateIlluminant(const Image& img, const std::vector<Vec3>& lin, const std::vector<uint8_t>& valid,
                        float& neutralShare, const std::vector<Vec3>* measure = nullptr);

// n evenly spaced quantiles (0, 1/(n-1), .. 1) of v
std::vector<float> evenQuantiles(std::vector<float> v, int n);

// Linear Rec.709 of a pixel after white balance and exposure normalisation by `amount`s (0..1).
struct Normaliser {
    Transfer transfer = Transfer::Rec709;
    Mat3 white{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    float gain = 1;
    Vec3 operator()(Vec3 code) const { return (white * decodeToRec709(transfer, code)) * gain; }
};
Normaliser makeNormaliser(const SceneAnalysis& a, float whiteAmount, float exposureAmount);

}  // namespace lm
