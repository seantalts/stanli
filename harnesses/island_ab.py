#!/usr/bin/env python3
"""The island table: what a compiled region costs with each backward.

For every posteriordb model that compiles a region, measure ns/gradient
three ways against the same build at the same point --

    off      islands disabled entirely (STANLI_NO_ISLAND=1)
    replay   islands on, backward re-executed under nested autodiff
    native   islands on, backward a generated adjoint program

-- and check that replay and native agree BITWISE on lp and every
gradient. The carve estimate is bypassed (STANLI_ISLAND_ALWAYS=1) so the
regions it declines are measured too: deciding whether it should still
decline them is the point of the table.

Usage: python3 harnesses/island_ab.py deps/posteriordb [--filter SUBSTR]
                                      [--timeout SEC]
Run from the worktree root with build-rel/ built.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path.cwd()
CHECK = REPO / "build-rel/stanli_check"
BENCH = REPO / "build-rel/bench_grad"
DUMP = REPO / "build-rel/dump_ops"
STANC = REPO / "deps/stanc3/stanc"

ALWAYS = {"STANLI_ISLAND_ALWAYS": "1"}
OFF = {"STANLI_NO_ISLAND": "1"}
REPLAY = {"STANLI_ISLAND_ALWAYS": "1", "STANLI_NO_NATIVE_ADJ": "1"}


def run(cmd, env=None, timeout=300):
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout, env=e)
    except subprocess.TimeoutExpired:
        return None


def evals_for(n):
    return 2000 if n < 100 else (400 if n < 1000 else 60)


def grad_of(stan, dj, env, timeout):
    """lp and gradients as exact %.17g strings, or None.

    stanli_check runs stanc itself, so this one takes the .stan source
    where the benchmarks below take the MIR.
    """
    r = run([str(CHECK), str(stan), str(dj)], env, timeout)
    if r is None or not r.stdout.startswith("OK "):
        return None
    return r.stdout.split()[1:]


def ns_grad(sexp, dj, n_params, env, timeout):
    """Minimum of three runs: a single run is noise (docs/lite-lp.md)."""
    best = None
    for _ in range(3):
        g = run([str(BENCH), str(sexp), str(dj), str(evals_for(n_params))],
                env, timeout)
        if g is None or not g.stdout.split():
            return None
        v = float(g.stdout.split()[0])
        best = v if best is None else min(best, v)
    return best


def n_ops(sexp, dj, env, timeout):
    r = run([str(DUMP), str(sexp), str(dj), "-1"], env, timeout)
    if r is None:
        return None
    for line in r.stdout.splitlines():
        if line.startswith("slots="):
            return int(line.split()[1].split("=")[1])
    return None


def main():
    pdb = pathlib.Path(sys.argv[1]) / "posterior_database"
    filt = (sys.argv[sys.argv.index("--filter") + 1]
            if "--filter" in sys.argv else "")
    timeout = int(sys.argv[sys.argv.index("--timeout") + 1]
                  if "--timeout" in sys.argv else 300)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_island_"))

    pairs = {}
    for pj in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(pj.read_text())
        pairs.setdefault(meta["model_name"], meta["data_name"])

    rows = []
    mismatched = []
    for model, dname in sorted(pairs.items()):
        if filt and filt not in model:
            continue
        stan = pdb / "models" / "stan" / f"{model}.stan"
        dz = pdb / "data" / "data" / f"{dname}.json.zip"
        if not stan.exists() or not dz.exists():
            continue
        sexp = tmp / f"{model}.sexp"
        r = run([str(STANC), "--debug-transformed-mir", str(stan)], None, timeout)
        if r is None or not r.stdout:
            continue
        sexp.write_text(r.stdout)
        dj = tmp / f"{model}.json"
        with zipfile.ZipFile(dz) as z:
            dj.write_bytes(z.read(z.namelist()[0]))

        # Does a region compile at all? An island shows up as ops the
        # passes-off graph does not have.
        dbg = run([str(DUMP), str(sexp), str(dj), "-1"], ALWAYS, timeout)
        if dbg is None or "ISLAND" not in dbg.stdout:
            continue

        base = grad_of(stan, dj, OFF, timeout)
        rep = grad_of(stan, dj, REPLAY, timeout)
        nat = grad_of(stan, dj, ALWAYS, timeout)
        if base is None or rep is None or nat is None:
            print(f"{model}: EVAL_FAIL", flush=True)
            continue
        bitwise = rep == nat
        if not bitwise:
            mismatched.append(model)
            worst, at = 0.0, -1
            for i, (a, b) in enumerate(zip(rep, nat)):
                fa, fb = float(a), float(b)
                d = abs(fa - fb) / max(abs(fa), 1e-300)
                if d > worst:
                    worst, at = d, i
            print(f"{model}: MISMATCH worst rel {worst:.2e} at {at}", flush=True)

        n_params = len(base) - 1
        off_ns = ns_grad(sexp, dj, n_params, OFF, timeout)
        rep_ns = ns_grad(sexp, dj, n_params, REPLAY, timeout)
        nat_ns = ns_grad(sexp, dj, n_params, ALWAYS, timeout)
        if None in (off_ns, rep_ns, nat_ns):
            print(f"{model}: BENCH_FAIL", flush=True)
            continue
        ops_off = n_ops(sexp, dj, OFF, timeout)
        ops_on = n_ops(sexp, dj, ALWAYS, timeout)
        rows.append((model, ops_off, ops_on, off_ns, rep_ns, nat_ns, bitwise))
        print(f"{model}: ops {ops_off}->{ops_on}  off {off_ns:.0f}  "
              f"replay {rep_ns:.0f} ({off_ns / rep_ns:.2f}x)  "
              f"native {nat_ns:.0f} ({off_ns / nat_ns:.2f}x)  "
              f"{'bitwise' if bitwise else 'MISMATCH'}", flush=True)

    rows.sort(key=lambda r: -(r[3] / r[5]))
    print("\n| model | ops off -> on | off | replay | native | |")
    print("| --- | ---: | ---: | ---: | ---: | ---: |")
    for m, oo, on, off, rep, nat, bw in rows:
        print(f"| `{m}` | {oo:,} -> {on:,} | {off:,.0f} | "
              f"{rep:,.0f} ({off / rep:.2f}x) | {nat:,.0f} "
              f"({off / nat:.2f}x) | {'' if bw else '**MISMATCH**'} |")
    print(f"\n{len(rows)} models compile a region; "
          f"{len(mismatched)} not bitwise: {mismatched}")


if __name__ == "__main__":
    main()
