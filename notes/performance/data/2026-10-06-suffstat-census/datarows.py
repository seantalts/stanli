#!/usr/bin/env python3
"""Data-level cross-check that does not use the op graph at all.

For each model: take every top-level data array whose length equals the most
common array length L >= 8 (rows of matrices count as one field each), zip
them into per-observation tuples and count distinct tuples.  This is an
upper bound on shape (b) for likelihoods the graph census cannot see (UDF
densities lowered into opaque islands, multivariate ops).
"""
import collections
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
status = json.loads((HERE / "stage1_status.json").read_text())
res = {}
for s in status:
    try:
        d = json.loads(pathlib.Path(s["data"]).read_text())
    except Exception:  # noqa: BLE001
        continue
    arrays = {k: v for k, v in d.items() if isinstance(v, list) and len(v) >= 8}
    if not arrays:
        res[s["name"]] = None
        continue
    L = collections.Counter(len(v) for v in arrays.values()).most_common(1)[0][0]
    cols = [v for v in arrays.values() if len(v) == L]
    rows = set()
    for i in range(L):
        rows.add(json.dumps([c[i] for c in cols]))
    res[s["name"]] = dict(L=L, fields=len(cols), distinct=len(rows))
(HERE / "datarows.json").write_text(json.dumps(res, indent=1))
ana = {r["name"]: r for r in json.loads((HERE / "analysis_with_estimates.json").read_text())}
print("models whose likelihood the graph census cannot see (no data-variate "
      "elementwise term with N>=8), with data rows L -> distinct:")
n_rep = 0
for name, r in sorted(ana.items()):
    big = [t for t in r["terms"] if t["variate_data"] and t["N"] >= 8]
    if big or not res.get(name):
        continue
    x = res[name]
    flag = "REPEATS" if x["distinct"] <= x["L"] / 2 else ""
    n_rep += bool(flag)
    print(f"  {name:28s} L={x['L']:6d} fields={x['fields']:2d} distinct={x['distinct']:6d} "
          f"target_terms={r['front_b'][0]:5d} {flag}")
print("with >=2x repeated data rows:", n_rep)
