#!/usr/bin/env python3
"""Turn consistency_tests --old output into the markdown table in docs/MATCHING.md.
  ./build/consistency_tests --old > run.txt; tools/consistency_table.py run.txt"""
import re, sys

rows = []
for line in open(sys.argv[1]):
    m = re.match(r'^(exposure|white balance|curves|saturation|log|combined)\s{2,}(.+?)\s{2,}(\d+\.\d+)\s+(\d+\.\d+)\s+(\d+\.\d+)?\s*(.*)$', line.rstrip())
    if not m:
        continue
    group, name, mean, p95, _, rest = m.groups()
    old = re.search(r'(\d+\.\d+) /\s+(\d+\.\d+)', rest)
    clipped = re.search(r'\(\+(\d+) clipped\)', rest)
    is_clipped = name.endswith('[clipped]')
    mean, p95 = float(mean), float(p95)
    ok = (mean <= 5 and p95 <= 12) if is_clipped else (mean <= 2 and p95 <= 5)
    rows.append((group, name.replace(' [clipped]', ' (clipped frames)'), mean, p95,
                 f"{old.group(1)} / {old.group(2)}" if old else "", "pass" if ok else "**fail**",
                 clipped.group(1) if clipped else ""))
print("| Group | Perturbation | Mean ΔE00 | p95 ΔE00 | Previous matcher (mean / p95) | Result |")
print("|---|---|---|---|---|---|")
for g, n, mean, p95, old, res, cl in rows:
    print(f"| {g} | {n}{f' ({cl} frames clipped, below)' if cl else ''} | {mean:.2f} | {p95:.2f} | {old} | {res} |")
