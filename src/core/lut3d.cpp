#include "lut3d.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "parallel.h"

namespace lm {

void Lut3D::fill(const std::function<Vec3(Vec3)>& f) {
    const float s = 1.0f / float(size_ - 1);
    std::vector<float> node(size_);
    for (int i = 0; i < size_; ++i) node[i] = shaper_.empty() ? i * s : unshape(i * s);
    parallelFor(0, size_, [&](int b) {
        for (int g = 0; g < size_; ++g)
            for (int r = 0; r < size_; ++r) at(r, g, b) = f({node[r], node[g], node[b]});
    });
}

float Lut3D::shape(float v) const {
    if (shaper_.empty()) return v;
    const int n = int(shaper_.size());
    float t = clamp01(v) * float(n - 1);
    int i = std::min(int(t), n - 2);
    return shaper_[i] + (t - i) * (shaper_[i + 1] - shaper_[i]);
}

float Lut3D::unshape(float u) const {  // inverse of shape (shaper is increasing)
    const int n = int(shaper_.size());
    if (u <= shaper_.front()) return 0.0f;
    if (u >= shaper_.back()) return 1.0f;
    int i = int(std::upper_bound(shaper_.begin(), shaper_.end(), u) - shaper_.begin()) - 1;
    i = std::clamp(i, 0, n - 2);
    float d = shaper_[i + 1] - shaper_[i];
    return (i + (d > 0 ? (u - shaper_[i]) / d : 0.0f)) / float(n - 1);
}

Vec3 Lut3D::apply(Vec3 c) const {
    if (!shaper_.empty()) c = {shape(c.r), shape(c.g), shape(c.b)};
    const float n = float(size_ - 1);
    float fr = clamp01(c.r) * n, fg = clamp01(c.g) * n, fb = clamp01(c.b) * n;
    int r0 = std::min(int(fr), size_ - 2), g0 = std::min(int(fg), size_ - 2), b0 = std::min(int(fb), size_ - 2);
    float dr = fr - r0, dg = fg - g0, db = fb - b0;
    const Vec3& c000 = at(r0, g0, b0);
    const Vec3& c111 = at(r0 + 1, g0 + 1, b0 + 1);
    // tetrahedral: pick the tetrahedron containing the point by ordering the fractions
    if (dr > dg) {
        if (dg > db) {         // r > g > b
            const Vec3 &c100 = at(r0 + 1, g0, b0), &c110 = at(r0 + 1, g0 + 1, b0);
            return c000 * (1 - dr) + c100 * (dr - dg) + c110 * (dg - db) + c111 * db;
        } else if (dr > db) {  // r > b > g
            const Vec3 &c100 = at(r0 + 1, g0, b0), &c101 = at(r0 + 1, g0, b0 + 1);
            return c000 * (1 - dr) + c100 * (dr - db) + c101 * (db - dg) + c111 * dg;
        } else {               // b > r > g
            const Vec3 &c001 = at(r0, g0, b0 + 1), &c101 = at(r0 + 1, g0, b0 + 1);
            return c000 * (1 - db) + c001 * (db - dr) + c101 * (dr - dg) + c111 * dg;
        }
    } else {
        if (db > dg) {         // b > g > r
            const Vec3 &c001 = at(r0, g0, b0 + 1), &c011 = at(r0, g0 + 1, b0 + 1);
            return c000 * (1 - db) + c001 * (db - dg) + c011 * (dg - dr) + c111 * dr;
        } else if (db > dr) {  // g > b > r
            const Vec3 &c010 = at(r0, g0 + 1, b0), &c011 = at(r0, g0 + 1, b0 + 1);
            return c000 * (1 - dg) + c010 * (dg - db) + c011 * (db - dr) + c111 * dr;
        } else {               // g > r > b
            const Vec3 &c010 = at(r0, g0 + 1, b0), &c110 = at(r0 + 1, g0 + 1, b0);
            return c000 * (1 - dg) + c010 * (dg - dr) + c110 * (dr - db) + c111 * db;
        }
    }
}

bool Lut3D::writeCube(const std::string& path, const std::string& title) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "TITLE \"%s\"\nLUT_3D_SIZE %d\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n", title.c_str(), size_);
    for (const Vec3& v : data_) std::fprintf(f, "%.6f %.6f %.6f\n", clamp01(v.r), clamp01(v.g), clamp01(v.b));
    return std::fclose(f) == 0;
}

bool Lut3D::readCube(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) { if (error) *error = "cannot open " + path; return false; }
    std::string line;
    int size = 0;
    std::vector<Vec3> vals;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "LUT_3D_SIZE") { ss >> size; vals.reserve(size_t(size) * size * size); continue; }
        if (key == "TITLE" || key == "DOMAIN_MIN" || key == "DOMAIN_MAX" || key == "LUT_1D_SIZE") {
            if (key == "LUT_1D_SIZE") { if (error) *error = "1D LUTs are not supported"; return false; }
            continue;
        }
        Vec3 v;
        std::istringstream ns(line);
        if (ns >> v.r >> v.g >> v.b) vals.push_back(v);
    }
    if (size < 2 || vals.size() != size_t(size) * size * size) {
        if (error) *error = "malformed .cube (size/entry count mismatch)";
        return false;
    }
    size_ = size;
    data_ = std::move(vals);
    return true;
}

}  // namespace lm
