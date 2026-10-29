// Film emulation: a two-stage negative -> print density model.
//
// Scene light hits the negative (per-layer H&D curves with interlayer coupling), the negative
// is printed onto print stock through printer lights, and the print's dye densities are
// projected. Each stage is a smooth sigmoid in log space, so the model has film's long toe and
// gentle shoulder, colour crossover between layers, and printer-point grading for free.
#pragma once
#include "color.h"

namespace lm {

struct NegativeStock {
    const char* name;
    float gamma[3];      // contrast per layer (R, G, B) in density per log exposure
    float speedOffset[3];// per-layer sensitivity shift in stops (colour cast / crossover)
    float coupling;      // interlayer inhibition: >0 raises saturation, <0 lowers it
    float grain;         // default grain amount 0..1
    float grainSize;     // default grain size multiplier
    float halation;      // default halation strength (remjet-less stocks are higher)
};

struct PrintStock {
    const char* name;
    float gamma[3];      // print contrast per layer
    float dmax[3];       // maximum density per layer (black depth / tint of blacks)
    float dyeCrosstalk;  // unwanted dye absorptions: adds print-like hue shifts
};

int negativeStockCount();
const NegativeStock& negativeStock(int i);
int printStockCount();
const PrintStock& printStock(int i);  // index 0 = "None (scanned negative)"

struct FilmSettings {
    int negative = 0;
    int print = 1;
    float printerPoints[3] = {0, 0, 0};  // R, G, B printer lights; +1 point = 0.025 log E
    float saturation = 1.0f;             // extra scale on interlayer coupling
};

class FilmModel {
public:
    explicit FilmModel(const FilmSettings& s);
    // scene linear Rec.709 primaries in, display-referred Rec.709 (gamma encoded, 0..1) out
    Vec3 apply(Vec3 linear) const;

private:
    Vec3 negativeDensity(Vec3 linear) const;
    Vec3 printTransmittance(Vec3 negDensity) const;

    NegativeStock neg_;
    PrintStock print_;
    bool scanOnly_;
    float printerLight_[3] = {0, 0, 0};  // calibrated so 18% grey prints neutral at the target density
    float coupling_;
    Vec3 greyNegDensity_;
    Vec3 crosstalkGrey_;  // crosstalk added at grey, subtracted so grey stays neutral
};

}  // namespace lm
