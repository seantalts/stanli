#!/usr/bin/env python3
"""Gradient agreement between original and hand-collapsed models (runtime)."""
import json
import math
import pathlib
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
CHECK = REPO / "build-spike/stanli_check"
status = {s["name"]: s for s in json.loads((HERE / "stage1_status.json").read_text())}


def grad(stan, mir, data, pt):
    out = subprocess.run([str(CHECK), str(stan), str(data), "--mir", str(mir),
                          "--point", str(pt)], capture_output=True, text=True).stdout
    return [float(x) for x in [ln for ln in out.splitlines()
                               if ln.startswith("OK")][-1].split()[2:]]


for orig, hand, data in (
        ("radon_county", "radon_county_suffstat", status["radon_county"]["data"]),
        ("ch12_m12_5", "ch12_m12_5_weighted", REPO / "tests/rethinking/ch12_m12_5.json")):
    for pt in (0, 1, 2):
        a = grad(HERE / "out" / orig / "model.stan", HERE / "out" / orig / "model.sexp",
                 data, pt)
        b = grad(HERE / "hand" / hand / "model.stan", HERE / "hand" / hand / "model.sexp",
                 HERE / "hand" / hand / "data.json", pt)
        big = max(abs(v) for v in a)
        unit = math.nextafter(big, math.inf) - big
        print(f"{orig} pt{pt}: n={len(a)} max|g|={big:.4g} max abs diff "
              f"{max(abs(x - y) for x, y in zip(a, b)):.3g} = "
              f"{max(abs(x - y) for x, y in zip(a, b)) / unit:.1f} ULP of the largest entry")
