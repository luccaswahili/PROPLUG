#!/usr/bin/env python3
"""LookMatch core engine: steal the look of a reference still and bake it into a 3D LUT.

Usage:
    python lookmatch.py REFERENCE.jpg FRAME.png [FRAME2.png ...] -o look.cube
                        [--strength 1.0] [--region-strength 1.0] [--skin-protect 0.5]
                        [--preview out.jpg]

REFERENCE  the still whose look you want
FRAME      one or more frames exported from your footage (File > Export > Frame in Premiere).
           Several frames from across a clip give a more stable match.

The .cube loads in Premiere via Lumetri Color > Creative > Look (or Basic Correction > Input LUT).

Pipeline, all in OKLab (perceptually uniform, so hue and lightness move independently):
  1. Regions: every colour gets soft membership in 8 colour regions (neutral, red, skin,
             yellow, green, cyan, blue, magenta). Statistics are content-balanced so a
             reference that is mostly sky does not drag the whole frame blue.
  2. Tone:   monotonic lightness curve from weighted histogram matching, smoothed.
  3. Color:  Monge-Kantorovich linear transfer of the (a, b) chroma distribution.
  4. Region match: each region present in both images is moved toward its counterpart
             (skin to skin, sky to sky, foliage to foliage).
  5. Split toning: per-band (shadows / mids / highlights) chroma offsets.
  6. Skin protect: skin colours keep their hue within a few degrees and a sane chroma range
             (automatically stronger when the reference has no skin to learn from).
  7. Bake:   evaluate on an N^3 RGB grid and write an Adobe/Resolve .cube.
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

# ---------- regions ----------

# (name, hue centre in degrees, hue sigma). OKLab hue of human skin sits at ~40-68 deg.
HUE_REGIONS = [("red", 15, 20), ("skin", 55, 16), ("yellow", 95, 20), ("green", 140, 25),
               ("cyan", 200, 25), ("blue", 260, 25), ("magenta", 320, 25)]
REGION_NAMES = ["neutral"] + [r[0] for r in HUE_REGIONS]
NEUTRAL_C = 0.025  # chroma below which a colour reads as grey

def region_weights(lab):
    """Soft membership (N, 8) that sums to 1 per colour; column 0 is neutral."""
    C = np.hypot(lab[:, 1], lab[:, 2])
    h = np.degrees(np.arctan2(lab[:, 2], lab[:, 1]))
    neutral = np.exp(-(C / NEUTRAL_C) ** 2)
    hw = []
    for _, c, s in HUE_REGIONS:
        d = (h - c + 180) % 360 - 180
        hw.append(np.exp(-(d ** 2) / (2 * s ** 2)))
    hw = np.stack(hw, axis=-1)
    hw /= hw.sum(axis=-1, keepdims=True) + 1e-12
    return np.concatenate([neutral[:, None], hw * (1 - neutral)[:, None]], axis=-1)

def skin_weight(lab):
    """How skin-like a colour is: skin hue, plausible chroma and lightness."""
    C = np.hypot(lab[:, 1], lab[:, 2])
    L = lab[:, 0]
    gate_c = np.clip((C - 0.015) / 0.02, 0, 1) * np.clip((0.20 - C) / 0.05, 0, 1)
    gate_l = np.clip((L - 0.2) / 0.1, 0, 1) * np.clip((0.95 - L) / 0.05, 0, 1)
    return region_weights(lab)[:, REGION_NAMES.index("skin")] * gate_c * gate_l

def balanced_weights(lab, power=0.5):
    """Per-pixel weights that reduce the pull of dominant regions (power 0 = off, 1 = equal)."""
    w = region_weights(lab)
    share = w.mean(0)
    pw = w @ (1.0 / np.maximum(share, 0.02) ** power)
    return pw / pw.mean(), w, share

# ---------- model ----------

def load(path, max_side=768):
    im = Image.open(path).convert("RGB")
    im.thumbnail((max_side, max_side), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64) / 255.0

def _sqrtm_psd(m):
    w, v = np.linalg.eigh(m)
    return (v * np.sqrt(np.clip(w, 1e-12, None))) @ v.T

def _wquantile(x, w, q):
    o = np.argsort(x)
    cw = np.cumsum(w[o])
    return np.interp(q, (cw - 0.5 * w[o]) / cw[-1], x[o])

def _wcov(x, w):
    m = (x * w[:, None]).sum(0) / w.sum()
    d = x - m
    return m, (d * w[:, None]).T @ d / w.sum()

BANDS = (0.2, 0.5, 0.8)  # shadow / mid / highlight centres in OKLab L
MIN_SHARE = 0.03         # a region must cover 3% of both images to be matched

def _band_weights(L):
    w = np.stack([np.exp(-((L - c) ** 2) / (2 * 0.15 ** 2)) for c in BANDS], axis=-1)
    return w / w.sum(axis=-1, keepdims=True)

class LookModel:
    def __init__(self, ref_rgb, src_rgb, strength=1.0, region_strength=1.0,
                 skin_protect=None, balance=0.5):
        ref = rgb_to_oklab(np.asarray(ref_rgb).reshape(-1, 3))
        src = rgb_to_oklab(np.asarray(src_rgb).reshape(-1, 3))
        self.strength = float(strength)
        self.region_strength = float(region_strength)

        bw_r, rw_r, share_r = balanced_weights(ref, balance)
        bw_s, rw_s, share_s = balanced_weights(src, balance)
        self.share_ref, self.share_src = share_r, share_s
        if skin_protect is None:  # auto: if the reference shows no skin, it can't teach us skin
            skin_protect = 0.3 if share_r[REGION_NAMES.index("skin")] >= MIN_SHARE else 0.8
        self.skin_protect = float(skin_protect)

        # compare like with like: global statistics come from regions both images contain,
        # unless they share too little to be meaningful
        # (each shared region counts equally, so the shift is the average per-region shift)
        shared = np.minimum(share_r, share_s) >= MIN_SHARE
        if shared.any() and min(share_r[shared].sum(), share_s[shared].sum()) >= 0.2:
            bw_r = rw_r[:, shared] @ (1.0 / share_r[shared]) + 1e-4
            bw_s = rw_s[:, shared] @ (1.0 / share_s[shared]) + 1e-4

        # 1. tone curve: weighted quantile mapping, sampled on 256 knots
        q = np.linspace(0, 1, 257)
        sq, rq = _wquantile(src[:, 0], bw_s, q), _wquantile(ref[:, 0], bw_r, q)
        sq = np.maximum.accumulate(sq + np.arange(len(sq)) * 1e-6)  # strictly increasing
        self.knots = np.linspace(0, 1, 256)
        curve = np.interp(self.knots, sq, rq)
        k = 9
        pad = np.pad(curve, k // 2, mode="edge")
        curve = np.convolve(pad, np.ones(k) / k, mode="valid")
        self.curve = np.maximum.accumulate(curve)

        # 2. chroma Monge-Kantorovich transfer on balanced statistics
        ms, cs = _wcov(src[:, 1:], bw_s)
        mr, cr = _wcov(ref[:, 1:], bw_r)
        cs, cr = cs + np.eye(2) * 1e-6, cr + np.eye(2) * 1e-6
        cs_h = _sqrtm_psd(cs)
        cs_hi = np.linalg.inv(cs_h)
        self.T = cs_hi @ _sqrtm_psd(cs_h @ cr @ cs_h) @ cs_hi
        self.ms, self.mr = ms, mr

        # 3. region match: per-region residual offsets (L, a, b), only where both images have it
        mapped = self._global(src)
        offs = np.zeros((len(REGION_NAMES), 3))
        for r in range(len(REGION_NAMES)):
            conf = min(share_r[r], share_s[r])
            if conf < MIN_SHARE:
                continue
            ws, wr = rw_s[:, r], rw_r[:, r]
            d = (ref * wr[:, None]).sum(0) / wr.sum() - (mapped * ws[:, None]).sum(0) / ws.sum()
            d[0] *= 0.5  # regions mostly carry colour; let the global curve own tone
            offs[r] = d * min(1.0, (conf - MIN_SHARE) / MIN_SHARE)  # fade in near threshold
        self.region_offsets = offs
        mapped = mapped + self.region_strength * (rw_s @ offs)

        # 4. split toning: residual chroma offsets per tonal band
        wm, wr = _band_weights(mapped[:, 0]) * bw_s[:, None], _band_weights(ref[:, 0]) * bw_r[:, None]
        boffs = []
        for b in range(len(BANDS)):
            if wm[:, b].sum() < 50 or wr[:, b].sum() < 50:
                boffs.append(np.zeros(2))
                continue
            m_ab = (mapped[:, 1:] * wm[:, b:b + 1]).sum(0) / wm[:, b].sum()
            r_ab = (ref[:, 1:] * wr[:, b:b + 1]).sum(0) / wr[:, b].sum()
            boffs.append(r_ab - m_ab)
        self.band_offsets = np.array(boffs)

    def _global(self, lab):
        out = np.empty_like(lab)
        out[:, 0] = np.interp(np.clip(lab[:, 0], 0, 1), self.knots, self.curve)
        out[:, 1:] = (lab[:, 1:] - self.ms) @ self.T.T + self.mr
        return out

    def _protect_skin(self, lab, out):
        s = self.skin_protect * skin_weight(lab)
        if not s.any():
            return out
        Ci, hi = np.hypot(lab[:, 1], lab[:, 2]), np.arctan2(lab[:, 2], lab[:, 1])
        Co, ho = np.hypot(out[:, 1], out[:, 2]), np.arctan2(out[:, 2], out[:, 1])
        dh = np.clip((ho - hi + np.pi) % (2 * np.pi) - np.pi, -np.radians(8), np.radians(8))
        Cp = np.clip(Co, 0.8 * Ci, 1.25 * Ci)
        hp = hi + dh
        prot = np.stack([Cp * np.cos(hp), Cp * np.sin(hp)], axis=-1)
        out = out.copy()
        out[:, 1:] += s[:, None] * (prot - out[:, 1:])
        return out

    def apply_lab(self, lab):
        out = self._global(lab)
        out += self.region_strength * (region_weights(lab) @ self.region_offsets)
        out[:, 1:] += _band_weights(out[:, 0]) @ self.band_offsets
        out = self._protect_skin(lab, out)
        return lab + self.strength * (out - lab)

    def apply_rgb(self, rgb):
        shp = rgb.shape
        return oklab_to_rgb(self.apply_lab(rgb_to_oklab(rgb.reshape(-1, 3)))).reshape(shp)

    def report(self):
        lines = [f"skin protect {self.skin_protect:.1f}", "region      ref%   src%   matched"]
        for i, n in enumerate(REGION_NAMES):
            m = "yes" if np.any(self.region_offsets[i]) else "-"
            lines.append(f"{n:<10}{100 * self.share_ref[i]:5.1f}  {100 * self.share_src[i]:5.1f}   {m}")
        return "\n".join(lines)

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
    ap.add_argument("frames", nargs="+")
    ap.add_argument("-o", "--out", default="look.cube")
    ap.add_argument("--strength", type=float, default=1.0, help="0 = no change, 1 = full match")
    ap.add_argument("--region-strength", type=float, default=1.0,
                    help="how hard skin/sky/foliage etc. are pulled to their counterparts (0 = off)")
    ap.add_argument("--skin-protect", type=float, default=None,
                    help="0 = skin follows the look, 1 = skin hue held natural "
                         "(default auto: 0.3, or 0.8 when the reference has no skin)")
    ap.add_argument("--balance", type=float, default=0.5,
                    help="content balancing, 0 = raw pixel counts, 1 = every region counts equally")
    ap.add_argument("--size", type=int, default=33, help="LUT grid size (17, 33 or 65)")
    ap.add_argument("--preview", help="write the first frame graded with the look")
    a = ap.parse_args()

    src = np.concatenate([load(p, 512).reshape(-1, 3) for p in a.frames])
    model = LookModel(load(a.reference), src, a.strength, a.region_strength, a.skin_protect, a.balance)
    print(model.report())
    write_cube(a.out, bake_lut(model, a.size), a.size)
    print(f"wrote {a.out}")
    if a.preview:
        f0 = np.asarray(Image.open(a.frames[0]).convert("RGB"), dtype=np.float64) / 255.0
        Image.fromarray((model.apply_rgb(f0) * 255 + 0.5).astype(np.uint8)).save(a.preview)
        print(f"wrote {a.preview}")

if __name__ == "__main__":
    main()
