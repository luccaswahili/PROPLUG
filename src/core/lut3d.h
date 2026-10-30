// 3D LUT with tetrahedral interpolation and Adobe/Resolve .cube I/O.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "color.h"

namespace lm {

class Lut3D {
public:
    Lut3D() = default;
    explicit Lut3D(int size) : size_(size), data_(size_t(size) * size * size) {}

    int size() const { return size_; }
    bool empty() const { return size_ < 2; }

    // index order matches .cube: red varies fastest
    Vec3& at(int r, int g, int b) { return data_[(size_t(b) * size_ + g) * size_ + r]; }
    const Vec3& at(int r, int g, int b) const { return data_[(size_t(b) * size_ + g) * size_ + r]; }

    // Fill every grid point from f(input rgb in 0..1). Runs on all cores. With a shaper, grid point k
    // holds f at the input the shaper sends to k/(size-1).
    void fill(const std::function<Vec3(Vec3)>& f);

    // Optional 1D shaper, the same for R, G and B, applied before the 3D lookup: kShaperSize samples
    // over 0..1 of a monotone curve from 0 to 1. It puts the grid's points where a frame's values
    // are (shadows of a dark clip, the middle band of a log clip), where a plain grid is too coarse.
    static constexpr int kShaperSize = 256;
    void setShaper(std::vector<float> s) { shaper_ = std::move(s); }
    const std::vector<float>& shaper() const { return shaper_; }
    float shape(float v) const;

    Vec3 apply(Vec3 c) const;  // (shaper, then) tetrahedral, input clamped to 0..1
    const Vec3* data() const { return data_.data(); }  // size^3 entries, red fastest (for GPU upload)

    bool writeCube(const std::string& path, const std::string& title) const;
    bool readCube(const std::string& path, std::string* error = nullptr);

private:
    float unshape(float u) const;
    int size_ = 0;
    std::vector<Vec3> data_;
    std::vector<float> shaper_;
};

}  // namespace lm
