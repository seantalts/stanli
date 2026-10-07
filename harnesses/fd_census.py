#!/usr/bin/env python3
"""Per-model census of the fused density ops, on the MIR ab_bench_corpus.py times.

python3 harnesses/fd_census.py PDB OUT.json --census BUILD/op_census

For every corpus model: lower the vectorizing-stanc MIR with op_census and
record, for each fused density, the number of bound ops and the longest
argument. OUT.json maps model -> {density: [ops, longest_argument]}.
"""
import argparse
import concurrent.futures
import json
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "harnesses"))
from corpus_bench import VECTORIZE_PROBE, benchmark_cases, materialize_data  # noqa: E402

FUSED = {"NORMAL_LPDF": "normal", "CAUCHY_LPDF": "cauchy",
         "STUDENT_T_LPDF": "student_t", "LOGNORMAL_LPDF": "lognormal",
         "BETA_LPDF": "beta", "GAMMA_LPDF": "gamma",
         "ORDERED_LOGISTIC_LPMF": "ordered_logistic"}
OP = re.compile(r"^OP_([A-Z0-9_]+) (\d+)((?: \d+)+)$")


def one(item, census, work):
    name, (stan, data) = item
    d = pathlib.Path(tempfile.mkdtemp(dir=work))
    mir = d / "model.sexp"
    dj = d / "data.json"
    materialize_data(data, dj)
    p = subprocess.run([str(VECTORIZE_PROBE), "--vectorize-loops", "on", "--output",
                        str(mir), str(stan)], capture_output=True, text=True, timeout=300)
    if p.returncode:
        return name, None
    r = subprocess.run([str(census), str(mir), str(dj)], capture_output=True,
                       text=True, timeout=300)
    if r.returncode:
        return name, None
    found = {}
    for line in r.stdout.splitlines():
        m = OP.match(line)
        if not m or m.group(1) not in FUSED:
            continue
        n_in = int(m.group(2))
        lens = [int(x) for x in m.group(3).split()]
        longest = max(lens[:n_in])
        e = found.setdefault(FUSED[m.group(1)], [0, 0])
        e[0] += 1
        e[1] = max(e[1], longest)
    return name, found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--census", type=pathlib.Path, required=True)
    ap.add_argument("--jobs", type=int, default=6)
    a = ap.parse_args()
    cases = benchmark_cases(a.pdb, "all")
    work = tempfile.mkdtemp()
    result, failed = {}, []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for name, found in pool.map(lambda it: one(it, a.census.resolve(), work),
                                    sorted(cases.items())):
            if found is None:
                failed.append(name)
            else:
                result[name] = found
    a.out.write_text(json.dumps(dict(models=result, failed=failed), indent=1))
    print(f"{len(cases)} models, {len(result)} lowered, {len(failed)} failed, "
          f"{sum(1 for v in result.values() if v)} with a fused density")


if __name__ == "__main__":
    main()
