#!/usr/bin/env python3
"""Ground truth for LookMatch's camera curves and gamuts: compare lookmatch-render --dump-transfers
with colour-science (BSD-3, test-only dependency: pip install colour-science).

  python tests/check_transfers.py build/lookmatch-render

For each log curve, the decode of 65 code values (relative error where linear > 0.002, absolute below),
and each gamut's matrix to linear Rec.709 (max element difference). Exits 1 if anything is off by more
than 0.5% / 1e-3.
"""
import json, subprocess, sys, warnings
warnings.filterwarnings("ignore")
import numpy as np
import colour
from colour import models as M

ours = json.loads(subprocess.run([sys.argv[1], "--dump-transfers"], capture_output=True, text=True, check=True).stdout)
codes = np.linspace(0, 1, 65)

curves = {
    "S-Log3": (lambda v: M.log_decoding_SLog3(v), "S-Gamut3.Cine"),
    "LogC3": (lambda v: M.log_decoding_ARRILogC3(v), "ARRI Wide Gamut 3"),
    "LogC4": (lambda v: M.log_decoding_ARRILogC4(v), "ARRI Wide Gamut 4"),
    "Canon Log 3": (lambda v: M.log_decoding_CanonLog3(v), "Cinema Gamut"),
    "V-Log": (lambda v: M.log_decoding_VLog(v), "V-Gamut"),
    "F-Log": (lambda v: M.log_decoding_FLog(v), "F-Gamut"),
    "F-Log2": (lambda v: M.log_decoding_FLog2(v), "F-Gamut"),
    "Apple Log": (lambda v: M.log_decoding_AppleLogProfile(v), "ITU-R BT.2020"),
    "BMD Film Gen5": (lambda v: M.oetf_inverse_BlackmagicFilmGeneration5(v), "Blackmagic Wide Gamut"),
    "Log3G10": (lambda v: M.log_decoding_Log3G10(v), "REDWideGamutRGB"),
    "D-Log": (lambda v: M.log_decoding_DJIDLog(v), "DJI D-Gamut"),
    "DaVinci Int.": (lambda v: M.oetf_inverse_DaVinciIntermediate(v), "DaVinci Wide Gamut"),
}
bad = 0
print(f"{'curve':14s} {'decode max err':>15s} {'at code':>8s}   {'gamut':24s} {'matrix max diff':>15s}")
for name, (dec, gamut) in curves.items():
    ref = np.asarray(dec(codes), dtype=np.float64)
    us = np.asarray(ours[name]["decode"], dtype=np.float64)
    err = np.where(np.abs(ref) > 0.002, np.abs(us - ref) / np.maximum(np.abs(ref), 1e-9), np.abs(us - ref))
    i = int(np.argmax(err))
    m_ref = colour.RGB_to_RGB(np.eye(3), colour.RGB_COLOURSPACES[gamut], colour.RGB_COLOURSPACES["ITU-R BT.709"],
                              chromatic_adaptation_transform="Bradford").T
    m_us = np.asarray(ours[name]["toRec709"])
    md = float(np.max(np.abs(m_ref - m_us)))
    flag = err[i] > 0.005 or md > 1e-3
    bad += flag
    print(f"{name:14s} {err[i]:15.5f} {codes[i]:8.3f}   {gamut:24s} {md:15.5f}{'   <-- OFF' if flag else ''}")
sys.exit(1 if bad else 0)
