#!/usr/bin/env python3
"""Throwaway: preparation time with and without the collapse, fast mode.
prep.py BENCH_GRAD WORK_DIR OUT.json [rounds]"""
import json, os, pathlib, statistics as st, subprocess, sys
bench, work, out = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3]
rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 7
res = {}
for d in sorted(work.iterdir()):
    mir, data = d / "model.sexp", d / "data.json"
    if not (mir.exists() and data.exists()): continue
    samples = {"off": [], "on": []}
    ok = True
    for r in range(rounds):
        for arm in (("off", "on") if r % 2 == 0 else ("on", "off")):
            env = dict(os.environ, STAN_NUM_THREADS="1")
            if arm == "off": env["STANLI_NO_COLLAPSE"] = "1"
            p = subprocess.run([bench, str(mir), str(data), "--prep", "--fast-math"],
                               capture_output=True, text=True, env=env)
            try: samples[arm].append(float(p.stdout.split()[0]))
            except Exception: ok = False
    if ok: res[d.name] = {a: min(v) for a, v in samples.items()}
json.dump(res, open(out, "w"), indent=0)
r = sorted((v["on"] / v["off"], v["on"] - v["off"], m, v["off"]) for m, v in res.items())
import math
print(len(r), "models; geomean on/off %.4f" % math.exp(sum(math.log(x[0]) for x in r) / len(r)))
print("total prep off %.3fs on %.3fs" % (sum(v["off"] for v in res.values()), sum(v["on"] for v in res.values())))
print("largest absolute increases:")
for x in sorted(r, key=lambda x: -x[1])[:12]: print("  %-40s %+8.2f ms  (%.2f ms -> x%.2f)" % (x[2], x[1]*1e3, x[3]*1e3, x[0]))
print("largest absolute decreases:")
for x in sorted(r, key=lambda x: x[1])[:8]: print("  %-40s %+8.2f ms  (%.2f ms -> x%.2f)" % (x[2], x[1]*1e3, x[3]*1e3, x[0]))
print("ratio > 1.10 and > 0.1 ms slower:", [(x[2], round(x[0],2), round(x[1]*1e3,2)) for x in r if x[0] > 1.10 and x[1] > 1e-4])
