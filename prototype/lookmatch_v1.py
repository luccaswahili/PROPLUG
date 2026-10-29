#!/usr/bin/env python3
"""LookMatch core engine: steal the look of a reference still and bake it into a 3D LUT.

Usage:
    python lookmatch.py REFERENCE.jpg SOURCE_FRAME.jpg -o look.cube [--strength 1.0] [--preview out.jpg]

REFERENCE     the still whose look you want
SOURCE_FRAME  a frame exported from your footage (File > Export > Frame in Premiere)

The .cube loads in Premiere via Lumetri Color > Creative > Look (or Basic Correction > Input LUT).

Pipeline, all in OKLab (perceptually uniform, so hue and lightness move independently):
  1. Tone:   monotonic lightness curve from histogram matching L(source) -> L(reference),
             smoothed and blended so it never posterizes.
  2. Color:  Monge-Kantorovich linear transfer of the (a, b) chroma distribution, which
             matches saturation, the dominant hue axis and its spread in one 2x2 matrix.
  3. Split toning: per-band (shadows / mids / highlights) chroma offsets, so a teal-shadow
             orange-highlight reference reproduces as such rather than a single global tint.
  4. Bake:   evaluate the transform on an N^3 RGB grid and write an Adobe/Resolve .cube.
"""
import argparse
import numpy as np
from PIL import Image

# ---------- color space ----------

def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)

_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])

def rgb_to_oklab(rgb):
    lms = srgb_to_linear(rgb) @ _M1.T
    return np.cbrt(lms) @ _M2.T

def oklab_to_rgb(lab):
    lms = (lab @ np.linalg.inv(_M2).T) ** 3
    return linear_to_srgb(lms @ np.linalg.inv(_M1).T)

# ---------- model ----------

def load(path, max_side=768):
    im = Image.open(path).convert("RGB")
    im.thumbnail((max_side, max_side), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64) / 255.0

def _sqrtm_psd(m):
    w, v = np.linalg.eigh(m)
    return (v * np.sqrt(np.clip(w, 1e-12, None))) @ v.T

BANDS = (0.2, 0.5, 0.8)  # shadow / mid / highlight centres in OKLab L

def _band_weights(L):
    w = np.stack([np.exp(-((L - c) ** 2) / (2 * 0.15 ** 2)) for c in BANDS], axis=-1)
    return w / w.sum(axis=-1, keepdims=True)

class LookModel:
    def __init__(self, ref_rgb, src_rgb, strength=1.0):
        ref = rgb_to_oklab(ref_rgb.reshape(-1, 3))
        src = rgb_to_oklab(src_rgb.reshape(-1, 3))
        self.strength = float(strength)

        # 1. tone curve: quantile mapping, sampled on 256 knots
        q = np.linspace(0, 1, 257)
        sq, rq = np.quantile(src[:, 0], q), np.quantile(ref[:, 0], q)
        sq = np.maximum.accumulate(sq + np.arange(len(sq)) * 1e-6)  # strictly increasing
        self.knots = np.linspace(0, 1, 256)
        curve = np.interp(self.knots, sq, rq)
        # smooth (box filter) and keep monotonic
        k = 9
        pad = np.pad(curve, k // 2, mode="edge")
        curve = np.convolve(pad, np.ones(k) / k, mode="valid")
        self.curve = np.maximum.accumulate(curve)

        # 2. chroma Monge-Kantorovich transfer
        ms, mr = src[:, 1:].mean(0), ref[:, 1:].mean(0)
        cs = np.cov(src[:, 1:].T) + np.eye(2) * 1e-6
        cr = np.cov(ref[:, 1:].T) + np.eye(2) * 1e-6
        cs_h = _sqrtm_psd(cs)
        cs_hi = np.linalg.inv(cs_h)
        self.T = cs_hi @ _sqrtm_psd(cs_h @ cr @ cs_h) @ cs_hi
        self.ms, self.mr = ms, mr

        # 3. split toning: residual chroma offsets per tonal band
        mapped = self._tone_color(src)
        wm, wr = _band_weights(mapped[:, 0]), _band_weights(ref[:, 0])
        offs = []
        for b in range(len(BANDS)):
            if wm[:, b].sum() < 50 or wr[:, b].sum() < 50:
                offs.append(np.zeros(2))  # not enough pixels in this band to trust
                continue
            m_ab = (mapped[:, 1:] * wm[:, b:b + 1]).sum(0) / wm[:, b].sum()
            r_ab = (ref[:, 1:] * wr[:, b:b + 1]).sum(0) / wr[:, b].sum()
            offs.append(r_ab - m_ab)
        self.band_offsets = np.array(offs)

    def _tone_color(self, lab):
        out = np.empty_like(lab)
        out[:, 0] = np.interp(np.clip(lab[:, 0], 0, 1), self.knots, self.curve)
        out[:, 1:] = (lab[:, 1:] - self.ms) @ self.T.T + self.mr
        return out

    def apply_lab(self, lab):
        out = self._tone_color(lab)
        out[:, 1:] += _band_weights(out[:, 0]) @ self.band_offsets
        return lab + self.strength * (out - lab)

    def apply_rgb(self, rgb):
        shp = rgb.shape
        return oklab_to_rgb(self.apply_lab(rgb_to_oklab(rgb.reshape(-1, 3)))).reshape(shp)

# ---------- LUT I/O ----------

def bake_lut(model, size=33):
    g = np.linspace(0, 1, size)
    # .cube order: red varies fastest
    b, gg, r = np.meshgrid(g, g, g, indexing="ij")
    grid = np.stack([r, gg, b], axis=-1).reshape(-1, 3)
    return np.clip(model.apply_rgb(grid), 0, 1)

def write_cube(path, table, size, title="LookMatch"):
    with open(path, "w") as f:
        f.write(f'TITLE "{title}"\nLUT_3D_SIZE {size}\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n')
        for r, g, b in table:
            f.write(f"{r:.6f} {g:.6f} {b:.6f}\n")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("source")
    ap.add_argument("-o", "--out", default="look.cube")
    ap.add_argument("--strength", type=float, default=1.0, help="0 = no change, 1 = full match")
    ap.add_argument("--size", type=int, default=33, help="LUT grid size (17, 33 or 65)")
    ap.add_argument("--preview", help="write the source frame graded with the LUT")
    a = ap.parse_args()

    model = LookModel(load(a.reference), load(a.source), a.strength)
    write_cube(a.out, bake_lut(model, a.size), a.size)
    print(f"wrote {a.out}")
    if a.preview:
        src = np.asarray(Image.open(a.source).convert("RGB"), dtype=np.float64) / 255.0
        Image.fromarray((model.apply_rgb(src) * 255 + 0.5).astype(np.uint8)).save(a.preview)
        print(f"wrote {a.preview}")

if __name__ == "__main__":
    main()
