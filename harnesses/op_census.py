#!/usr/bin/env python3
"""Where is scalar work left? Lower every posteriordb model and report the
count of ops whose output is a single element -- the ops still paying the
interpreter's per-op dispatch + recorder tax per element of data. Ranks
models by remaining scalar ops and aggregates the responsible opcodes, so
the next vectorization target is chosen from the corpus, not from memory.

Usage: python3 harnesses/op_census.py deps/posteriordb [--filter SUBSTR]
Needs build-rel/ built.
"""
import collections
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
DUMP = REPO / "build-rel/dump_ops"
STANC = REPO / "deps/stanc3/stanc"
TIMEOUT = 600


def run(cmd, timeout=TIMEOUT):
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout, env=dict(os.environ))
    except subprocess.TimeoutExpired:
        return None


def main():
    pdb = pathlib.Path(sys.argv[1]) / "posterior_database"
    filt = (sys.argv[sys.argv.index("--filter") + 1]
            if "--filter" in sys.argv else "")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_census_"))

    pairs = {}
    for pj in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(pj.read_text())
        pairs.setdefault(meta["model_name"], meta["data_name"])

    rows = []
    by_op = collections.Counter()      # scalar-out ops, corpus wide
    by_op_models = collections.Counter()
    skipped = []
    for model, dname in sorted(pairs.items()):
        if filt and filt not in model:
            continue
        stan = pdb / "models" / "stan" / f"{model}.stan"
        dz = pdb / "data" / "data" / f"{dname}.json.zip"
        if not stan.exists() or not dz.exists():
            continue
        dj = tmp / f"{model}.json"
        with zipfile.ZipFile(dz) as z:
            dj.write_bytes(z.read(z.namelist()[0]))
        r = run([str(STANC), "--O1", "--debug-optimized-mir", str(stan)])
        if r is None or r.returncode != 0:
            skipped.append(f"{model}: stanc")
            continue
        sexp = tmp / f"{model}.sexp"
        sexp.write_text(r.stdout)
        d = run([str(DUMP), str(sexp), str(dj), "-1"])
        if d is None or d.returncode != 0:
            skipped.append(f"{model}: {'timeout' if d is None else 'lower'}")
            continue
        total = scalar = 0
        ops_here = []
        for line in d.stdout.splitlines():
            f = line.split()
            if line.startswith("SUMMARY"):
                total = int(f[1].split("=")[1])
                scalar = int(f[2].split("=")[1])
            elif line.startswith("  ") and "scalar=" in line:
                name = f[0]
                n_scalar = int(f[2].split("=")[1])
                if n_scalar:
                    ops_here.append((name, n_scalar))
        rows.append((model, total, scalar))
        for name, n in ops_here:
            by_op[name] += n
            by_op_models[name] += 1

    print(f"\n{'model':42s} {'ops':>9s} {'scalar-out':>11s} {'%':>5s}")
    for m, total, scalar in sorted(rows, key=lambda r: -r[2])[:20]:
        pct = 100.0 * scalar / total if total else 0.0
        print(f"{m:42s} {total:9d} {scalar:11d} {pct:5.0f}")

    print(f"\ncorpus totals: {len(rows)} models, "
          f"{sum(r[1] for r in rows):,} ops, "
          f"{sum(r[2] for r in rows):,} scalar-out")
    print("\nopcodes by scalar-output count (corpus wide):")
    for name, n in by_op.most_common(14):
        print(f"  {name:24s} {n:9d}  in {by_op_models[name]} models")
    if skipped:
        print(f"\nskipped {len(skipped)}: {', '.join(skipped[:8])}"
              f"{' ...' if len(skipped) > 8 else ''}")


if __name__ == "__main__":
    main()
