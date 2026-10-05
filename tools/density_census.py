#!/usr/bin/env python3
"""Count probability functions across the corpus.

For every corpus model, compile the source with stanc (--O1
--debug-optimized-mir) and count the density and probability function call
sites in the MIR, then lower the model with tools/op_census and count the
bound ops that implement them, with the lengths of their arguments.

  tools/density_census.py deps/posteriordb --census build/op_census \
      --stanc deps/stanc3/stanc [--json out.json]

Columns: models (models that call the function), mir_sites (call sites in
the MIR), ops (bound ops, one gradient runs each once), vector_ops (bound
ops with an argument of length above one), elements (sum over ops of the
longest argument, the amount of data the call reads per gradient).
"""
import argparse
import collections
import concurrent.futures
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

from corpus_inventory import corpus_cases, materialize_data

PROB = re.compile(r"(?:_lpdf|_lpmf|_lupdf|_lupmf|_cdf|_lcdf|_lccdf)$")
MIR_CALL = re.compile(r"\(StanLib ([a-z0-9_]+)")
OP_PROB = re.compile(r"^OP_([A-Z0-9_]+?)_(LPDF|LPMF|CDF|LCDF|LCCDF)$")


def normalize(name):
    return re.sub(r"_lu(p[dm]f)$", r"_l\1", name)


def one(case, stanc, census, work):
    directory = pathlib.Path(tempfile.mkdtemp(dir=work))
    try:
        stan = directory / "model.stan"
        shutil.copyfile(case.source, stan)
        mir = subprocess.run(
            [str(stanc), "--O1", "--debug-optimized-mir", stan.name],
            cwd=directory, capture_output=True, text=True, timeout=120)
        if mir.returncode:
            return case.name, None, None
        (directory / "model.tmir.sexp").write_text(mir.stdout)
        sites = collections.Counter(
            normalize(n) for n in MIR_CALL.findall(mir.stdout) if PROB.search(n))
        data = materialize_data(case, directory)
        run = subprocess.run(
            [str(census), str(directory / "model.tmir.sexp"), str(data)],
            capture_output=True, text=True, timeout=300)
        ops = []
        if run.returncode == 0:
            for line in run.stdout.splitlines():
                p = line.split()
                m = OP_PROB.match(p[0])
                if m:
                    n_in = int(p[1])
                    lens = [int(x) for x in p[2:2 + n_in]]
                    ops.append((m.group(1).lower() + "_" + m.group(2).lower(),
                                lens, int(p[2 + n_in])))
        return case.name, sites, (ops if run.returncode == 0 else None)
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("--census", type=pathlib.Path, required=True)
    ap.add_argument("--stanc", type=pathlib.Path, required=True)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--json", type=pathlib.Path)
    a = ap.parse_args()
    cases = corpus_cases(a.pdb, "all")
    work = tempfile.mkdtemp()
    rows = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        futures = [pool.submit(one, c, a.stanc.resolve(), a.census.resolve(),
                               work) for c in cases.values()]
        for f in futures:
            name, sites, ops = f.result()
            rows[name] = (sites, ops)
    shutil.rmtree(work, ignore_errors=True)

    compiled = {n for n, (s, _) in rows.items() if s is not None}
    lowered = {n for n, (_, o) in rows.items() if o is not None}
    stats = collections.defaultdict(lambda: collections.Counter())
    for name, (sites, ops) in rows.items():
        for fn, n in (sites or {}).items():
            stats[fn]["mir_sites"] += n
            stats[fn]["models"] += 1
        for fn, lens, out in ops or []:
            s = stats[fn]
            s["ops"] += 1
            s["vector_ops"] += max(lens + [out]) > 1
            s["elements"] += max(lens + [out])
    print(f"{len(cases)} models, {len(compiled)} compiled, "
          f"{len(lowered)} lowered", file=sys.stderr)
    order = sorted(stats, key=lambda k: -stats[k]["elements"])
    print(f"{'function':28s} {'models':>6s} {'mir_sites':>9s} {'ops':>8s} "
          f"{'vector_ops':>10s} {'elements':>10s}")
    for fn in order:
        s = stats[fn]
        print(f"{fn:28s} {s['models']:6d} {s['mir_sites']:9d} {s['ops']:8d} "
              f"{s['vector_ops']:10d} {s['elements']:10d}")
    if a.json:
        a.json.write_text(json.dumps(
            {"models": len(cases), "compiled": len(compiled),
             "lowered": len(lowered),
             "functions": {k: dict(v) for k, v in stats.items()}}, indent=1))


if __name__ == "__main__":
    main()
