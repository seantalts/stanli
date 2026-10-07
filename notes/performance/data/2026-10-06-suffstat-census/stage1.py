#!/usr/bin/env python3
"""Stage 1 of the sufficient-statistics census (throwaway spike script).

For every corpus model: produce MIR with the pinned stanli compiler probe,
materialize data, and dump the bound log-prob graph (a) in the shipped
configuration and (b) in the "census" configuration, where the passes that
hide scalar structure inside opaque ops are switched off.

  python3 scratch/suffstat/stage1.py [--jobs 8] [--only name,name]
"""
import argparse
import concurrent.futures
import gzip
import json
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "tools"))
from corpus_inventory import corpus_cases, materialize_data  # noqa: E402

PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
DUMP = HERE / "graph_dump"
OUT = HERE / "out"
CENSUS_ENV = {
    "STANLI_NO_ISLAND": "1",
    "STANLI_NO_PARTITION": "1",
    "STANLI_NO_CSE": "1",
    "STANLI_REGION_MAP": "0",
    "STANLI_STRUCTURED_LOOPS": "0",
    "STANLI_BOUNDED_SPECIALIZATION": "0",
}


def one(case):
    d = OUT / case.name
    d.mkdir(parents=True, exist_ok=True)
    status = {"name": case.name, "collection": case.collection}
    src = d / "model.stan"
    shutil.copyfile(case.source, src)
    mir = d / "model.sexp"
    try:
        r = subprocess.run([str(PROBE), "--vectorize-loops", "on", "--output",
                            str(mir), str(src)], capture_output=True, text=True,
                           timeout=300)
    except subprocess.TimeoutExpired:
        status["mir"] = "timeout"
        return status
    if r.returncode:
        status["mir"] = "fail: " + r.stderr.strip()[-300:]
        return status
    status["mir"] = "ok"
    data = materialize_data(case, d)
    status["data"] = str(data)
    for tag, extra in (("default", {}), ("census", CENSUS_ENV)):
        env = dict(os.environ)
        env.update(extra)
        try:
            r = subprocess.run([str(DUMP), str(mir), str(data)],
                               capture_output=True, env=env, timeout=900)
        except subprocess.TimeoutExpired:
            status[tag] = "timeout"
            continue
        if r.returncode:
            status[tag] = "fail: " + r.stderr.decode(errors="replace")[-300:]
            continue
        with gzip.open(d / f"{tag}.txt.gz", "wb", compresslevel=1) as f:
            f.write(r.stdout)
        status[tag] = "ok"
        if r.stderr.strip():
            status[tag + "_stderr"] = r.stderr.decode(errors="replace")[-300:]
    return status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only")
    a = ap.parse_args()
    cases = corpus_cases(REPO / "deps/posteriordb", "all")
    if a.only:
        cases = {k: v for k, v in cases.items() if k in a.only.split(",")}
    OUT.mkdir(exist_ok=True)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for st in pool.map(one, cases.values()):
            rows.append(st)
            print(st["name"], st.get("mir"), st.get("default"), st.get("census"),
                  flush=True)
    (HERE / "stage1_status.json").write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
