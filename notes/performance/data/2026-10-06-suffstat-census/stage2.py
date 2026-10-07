#!/usr/bin/env python3
"""Stage 2: per-opcode gradient profiles (throwaway spike script).

For every model with a stage-1 dump, run bench_grad with STANLI_PROFILE=1 in
the census configuration (profile opcodes then match the census graph) and
in the shipped configuration, plus one unprofiled shipped-configuration run.
Timings are noisy (shared machine); only shares are used downstream.
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from stage1 import CENSUS_ENV, OUT  # noqa: E402

BENCH = REPO / "build-spike/bench_grad"


def run(mir, data, n, extra, profile):
    env = dict(os.environ)
    env.update(extra)
    if profile:
        env["STANLI_PROFILE"] = "1"
    r = subprocess.run([str(BENCH), str(mir), str(data), str(n)],
                       capture_output=True, text=True, env=env, timeout=600)
    if r.returncode:
        raise RuntimeError(r.stderr.strip()[-300:])
    ns = float(r.stdout.split()[0])
    prof = {}
    total = None
    for line in r.stderr.splitlines():
        p = line.split()
        if len(p) == 6 and p[0].startswith("OP_"):
            prof[p[0]] = dict(calls=int(p[1]), fwd=int(p[2]), bwd=int(p[3]),
                              elems=int(p[5]))
        elif len(p) == 3 and p[1] == "ns" and p[2] == "total":
            total = int(p[0])
    return ns, prof, total


def one(st):
    d = OUT / st["name"]
    mir, data = d / "model.sexp", st["data"]
    res = {"name": st["name"]}
    try:
        ns, _, _ = run(mir, data, 3, {}, False)
        n = int(min(20000, max(5, 4e8 / max(ns, 1.0))))
        res["n"] = n
        res["default_ns"], _, _ = run(mir, data, n, {}, False)
        _, res["default_prof"], res["default_prof_total"] = run(mir, data, n, {}, True)
        res["census_ns"], _, _ = run(mir, data, n, CENSUS_ENV, False)
        _, res["census_prof"], res["census_prof_total"] = run(
            mir, data, n, CENSUS_ENV, True)
        res["status"] = "ok"
    except Exception as e:  # noqa: BLE001
        res["status"] = f"fail: {e}"[:400]
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--only")
    a = ap.parse_args()
    sts = [s for s in json.loads((HERE / "stage1_status.json").read_text())
           if s.get("census") == "ok"]
    if a.only:
        sts = [s for s in sts if s["name"] in a.only.split(",")]
    rows = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for r in pool.map(one, sts):
            rows.append(r)
            print(r["name"], r["status"], r.get("default_ns"), r.get("census_ns"),
                  flush=True)
    (HERE / "stage2_profiles.json").write_text(json.dumps(rows))


if __name__ == "__main__":
    main()
