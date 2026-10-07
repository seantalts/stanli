#!/usr/bin/env python3
"""Stage 2b: unprofiled ns/gradient, shipped default vs runtime fast-math.

The MIR is the default-mode MIR (the pinned compiler probe in deps/ predates
--fast-math), so "fast" here is the runtime half of fast mode only: merged
active duplicates in CSE and fusion over shared parameters.  Two alternating
rounds, minimum kept.  Noisy: another benchmark shares the machine.
"""
import concurrent.futures
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from stage1 import OUT  # noqa: E402

BENCH = REPO / "build-spike/bench_grad"


def ns(mir, data, n, fast):
    cmd = [str(BENCH), str(mir), str(data), str(n)] + (["--fast-math"] if fast else [])
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if r.returncode:
        raise RuntimeError(r.stderr.strip()[-200:])
    return float(r.stdout.split()[0])


def one(p):
    st = STATUS[p["name"]]
    mir, data = OUT / p["name"] / "model.sexp", st["data"]
    res = {"name": p["name"]}
    try:
        d, f = [], []
        for _ in range(2):
            d.append(ns(mir, data, p["n"], False))
            f.append(ns(mir, data, p["n"], True))
        res.update(default_ns=min(d), fast_ns=min(f), status="ok")
    except Exception as e:  # noqa: BLE001
        res["status"] = f"fail: {e}"[:300]
    return res


STATUS = {s["name"]: s for s in json.loads((HERE / "stage1_status.json").read_text())}
profs = [p for p in json.loads((HERE / "stage2_profiles.json").read_text())
         if p.get("status") == "ok"]
rows = []
with concurrent.futures.ThreadPoolExecutor(6) as pool:
    for r in pool.map(one, profs):
        rows.append(r)
        print(r["name"], r["status"], r.get("default_ns"), r.get("fast_ns"), flush=True)
(HERE / "stage2b_fast.json").write_text(json.dumps(rows))
