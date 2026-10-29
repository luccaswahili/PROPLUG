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

    // Fill every grid point from f(input rgb in 0..1). Runs on all cores.
    void fill(const std::function<Vec3(Vec3)>& f);

    Vec3 apply(Vec3 c) const;  // tetrahedral, input clamped to 0..1
    const Vec3* data() const { return data_.data(); }  // size^3 entries, red fastest (for GPU upload)

    bool writeCube(const std::string& path, const std::string& title) const;
    bool readCube(const std::string& path, std::string* error = nullptr);

private:
    int size_ = 0;
    std::vector<Vec3> data_;
};

}  // namespace lm
