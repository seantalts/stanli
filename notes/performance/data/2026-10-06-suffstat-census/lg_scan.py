#!/usr/bin/env python3
"""cond(Z) for every linear-Gaussian term found by analyze2 (throwaway)."""
import json
import pathlib
import sys

import numpy as np

import analyze2 as A2

HERE = pathlib.Path(__file__).resolve().parent
names = sys.argv[1:] or [r["name"] for r in json.loads((HERE / "analysis2.json").read_text())
                         if any(not g["lognormal"] and g["N"] >= 8 for g in r.get("lg", []))]
out = []
for nm in names:
    A = A2.Analysis2(HERE / "out" / nm / "census.txt.gz")
    A.prepare()
    gs = [g for g in A.lg_groups if not g["lognormal"] and g["N"] >= 8]
    if not gs:
        continue
    g = max(gs, key=lambda g: g["N"])
    if g["N"] * g["P"] > 3e7:
        continue
    Z = np.zeros((g["N"], g["P"]))
    r, c, v = g["_Z"]
    Z[r, c] = v
    sv = np.linalg.svd(Z, compute_uv=False)
    rank = int((sv > sv[0] * 1e-12).sum())
    Zs = Z / np.maximum(np.linalg.norm(Z, axis=0), 1e-300)
    sv2 = np.linalg.svd(Zs, compute_uv=False)
    row = dict(name=nm, N=g["N"], P=g["P"], rank=rank,
               cond=float(sv[0] / sv[min(g["N"], g["P"]) - 1]) if sv[-1] > 0 else float("inf"),
               cond_scaled=float(sv2[0] / sv2[-1]) if sv2[-1] > 0 else float("inf"))
    out.append(row)
    print(f"{nm:38s} N={row['N']:6d} P={row['P']:5d} rank={rank:5d} cond={row['cond']:.3g} "
          f"cond(col-scaled)={row['cond_scaled']:.3g}", flush=True)
(HERE / "lg_cond.json").write_text(json.dumps(out, indent=1))
