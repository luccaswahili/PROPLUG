#!/usr/bin/env python3
"""Benchmark baseline: the color-matcher package (hahnec/color-matcher, GPL-3.0, used here only as an
external test tool, never shipped or linked) on the frames `consistency_tests --dump DIR` writes.

  pip install color-matcher colour-science   (in a throwaway venv)
  consistency_tests --max 8 --dump DIR
  python tools/colormatcher_bench.py DIR REFERENCE.png [method]   (default method hm-mvgd-hm)

For each frame it matches the untouched frame and every perturbed copy to the reference and reports,
per perturbation, the mean and 95th percentile CIEDE2000 between the perturbed result and the untouched
result: the same consistency measure as consistency_tests (frames where the perturbation clipped more than
1% more of the pixels are reported separately as [clipped]).
"""
import glob, os, sys, warnings
warnings.filterwarnings("ignore")
import numpy as np
import colour
from color_matcher import ColorMatcher
from color_matcher.io_handler import load_img_file

d, ref_path = sys.argv[1], sys.argv[2]
method = sys.argv[3] if len(sys.argv) > 3 else "hm-mvgd-hm"
names = {}
for line in open(os.path.join(d, "perturbations.tsv")):
    k, group, name, _ = line.rstrip("\n").split("\t")
    names[k] = (group, name)

def load(p):
    return load_img_file(p).astype(np.float64)[..., :3]  # floats 0..1, no alpha

ref = load(ref_path)
if ref.shape[1] > 480:
    s = ref.shape[1] // 480
    ref = ref[::s, ::s]
cm = ColorMatcher()

def match(img):
    return np.clip(cm.transfer(src=img, ref=ref, method=method), 0, 1)

def lab(img):
    return colour.XYZ_to_Lab(colour.sRGB_to_XYZ(img))

def clipped(img):
    return float(np.mean(np.any((img >= 254 / 255) | (img <= 1 / 255), axis=-1)))

rows = {}
for base in sorted(glob.glob(os.path.join(d, "*__untouched.png"))):
    stem = base[: -len("__untouched.png")]
    F = load(base)
    out0 = lab(match(F))
    c0 = clipped(F)
    for k, (group, name) in names.items():
        p = f"{stem}__p{k}.png"
        if not os.path.exists(p):
            continue
        Fp = load(p)
        de = colour.delta_E(lab(match(Fp)), out0, method="CIE 2000").ravel()
        key = (group, name + (" [clipped]" if clipped(Fp) > c0 + 0.01 else ""))
        rows.setdefault(key, []).append((de.mean(), np.percentile(de, 95)))

print(f"{'group':14s} {'perturbation':44s} {'mean':>6s} {'p95':>7s}   ({method})")
for (group, name), v in rows.items():
    v = np.array(v)
    print(f"{group:14s} {name:44s} {v[:, 0].mean():6.2f} {v[:, 1].mean():7.2f}")
