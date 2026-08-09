#!/usr/bin/env python3
"""Full-corpus A/B for the graph passes: for every posteriordb (model,
data) pair, compare stanli_check output (lp + gradient at the
deterministic point) with the passes disabled vs enabled, and count ops
via dump_ops. The passes-off graph is the CmdStan-verified baseline, so
A/B parity is transitive verification of the passes.

Usage: python3 harnesses/ab_corpus.py deps/posteriordb [--filter SUBSTR]
                                   [--disable VAR[,VAR...]]
Default disables every pass at once (re-roll, in-place, constant
folding, islands), which is the end-to-end check; pass one variable to
attribute a divergence.
Needs build-rel/ built.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
CHECK = REPO / "build-rel/stanli_check"
DUMP = REPO / "build-rel/dump_ops"
STANC = REPO / "deps/stanc3/stanc"
TIMEOUT = 300


def run(cmd, env=None, timeout=TIMEOUT):
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout, env=e)
    except subprocess.TimeoutExpired:
        return None


def main():
    pdb = pathlib.Path(sys.argv[1]) / "posterior_database"
    filt = (sys.argv[sys.argv.index("--filter") + 1]
            if "--filter" in sys.argv else "")
    # Each entry is VAR or VAR=VALUE (default value "1").
    disable = {}
    for v in (sys.argv[sys.argv.index("--disable") + 1].split(",")
              if "--disable" in sys.argv
              else ["STANLI_NO_REROLL", "STANLI_NO_INPLACE",
                    "STANLI_NO_CONSTFOLD", "STANLI_NO_ISLAND"]):
        k, _, val = v.partition("=")
        disable[k] = val or "1"
    print(f"A = passes off ({', '.join(disable)}), B = passes on")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_ab_"))

    pairs = {}
    for pj in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(pj.read_text())
        pairs.setdefault(meta["model_name"], meta["data_name"])

    n_same_fail = n_ok_same_graph = n_ok_rerolled = 0
    worst = (0.0, "")
    rerolled = []
    flags = []
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

        a = run([str(CHECK), str(stan), str(dj)], disable)
        b = run([str(CHECK), str(stan), str(dj)])
        if a is None or b is None:
            flags.append(f"{model}: TIMEOUT (a={'T' if a is None else 'ok'})")
            continue
        at, bt = a.stdout.split(), b.stdout.split()
        if not at or not bt:
            flags.append(f"{model}: EMPTY OUTPUT")
            continue
        if at[0] != "OK" or bt[0] != "OK":
            if at[:1] == bt[:1]:
                n_same_fail += 1  # both fail identically: known gap
            else:
                flags.append(f"{model}: STATUS DIVERGED "
                             f"nopass={at[0]} pass={bt[0]}")
            continue
        va = [float(t) for t in at[1:]]
        vb = [float(t) for t in bt[1:]]
        if len(va) != len(vb):
            flags.append(f"{model}: GRADIENT LENGTH DIVERGED")
            continue
        dev = max((abs(x - y) / max(abs(x), 1e-300)
                   for x, y in zip(va, vb)), default=0.0)
        if dev > worst[0]:
            worst = (dev, model)
        if dev > 1e-11:
            flags.append(f"{model}: DEVIATION {dev:.2e}")

        # op counts (reuse one stanc run for both dumps)
        sexp = tmp / f"{model}.sexp"
        r = run([str(STANC), "--debug-transformed-mir", str(stan)])
        ops_a = ops_b = -1
        if r is not None and r.returncode == 0:
            sexp.write_text(r.stdout)
            da = run([str(DUMP), str(sexp), str(dj), "0"], disable)
            db = run([str(DUMP), str(sexp), str(dj), "0"])
            if da and db and da.returncode == 0 and db.returncode == 0:
                ops_a = int(da.stdout.split()[1].split("=")[1])
                ops_b = int(db.stdout.split()[1].split("=")[1])
        if ops_b >= 0 and ops_b < ops_a:
            n_ok_rerolled += 1
            rerolled.append((model, ops_a, ops_b, dev))
        else:
            n_ok_same_graph += 1

    print(f"\nboth-fail (known gaps): {n_same_fail}")
    print(f"OK, graph untouched:    {n_ok_same_graph}")
    print(f"OK, graph changed:      {n_ok_rerolled}")
    print(f"worst deviation:        {worst[0]:.2e} ({worst[1]})")
    print("\nmodels changed by the passes (ops before -> after, deviation):")
    for m, oa, ob, dev in sorted(rerolled, key=lambda r: r[1] - r[2],
                                 reverse=True):
        print(f"  {m:40s} {oa:7d} -> {ob:6d}  {dev:.2e}")
    if flags:
        print("\nFLAGS (investigate):")
        for f in flags:
            print(f"  {f}")
        sys.exit(1)
    print("\nno flags.")


if __name__ == "__main__":
    main()
