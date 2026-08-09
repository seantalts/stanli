#!/usr/bin/env python3
"""Sampler-level differential harness: stanli's NUTS trace vs CmdStan's.

The gradient rig (tools/verify_sample.py) compares log density and
gradients at fixed points. It is structurally blind to anything about how
the sampler is *configured*, because configuration never enters a
pointwise gradient. That blind spot shipped a real bug: run_nuts never
called set_max_depth, so trajectories capped at 31 leapfrogs instead of
1023, every model needing deep trees was silently under-explored, and
every gradient still verified.

This is the oracle for that class. Same model, same data, same seed,
same warmup and sample counts; compare the distributions of the sampler
columns CmdStan writes. Trajectories diverge between engines after the
first accepted proposal (different RNG streams), so the comparison is
distributional, not pointwise: total leapfrogs, mean and max tree depth,
adapted stepsize, divergence rate, and the lp__ distribution.

Usage:
  tools/sampler_trace.py CMDSTAN_DIR PDB_DIR model [model ...]
  tools/sampler_trace.py --stan model.stan --data data.json CMDSTAN_DIR

Options:
  --warmup N --samples N --seed N     sampler settings for both engines
  --tol-leapfrog F   allowed relative difference in total leapfrogs (0.5)
  --tol-stepsize F   allowed relative difference in adapted stepsize (0.5)
  --tol-lp F         allowed |mean lp difference| in units of stanli's
                     lp standard deviation (0.5)

Exit nonzero when any model exceeds a tolerance. The tolerances are wide
on purpose: this is looking for configuration divergence (a 30x leapfrog
gap, an order-of-magnitude stepsize gap), not for Monte Carlo noise.
"""
import argparse
import json
import math
import pathlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
COLS = ["lp__", "accept_stat__", "stepsize__", "treedepth__",
        "n_leapfrog__", "divergent__", "energy__"]


def read_csv(path_or_text, is_text=False):
    """CmdStan-style CSV: # comments, one header row, numeric rows."""
    text = path_or_text if is_text else pathlib.Path(path_or_text).read_text()
    header, rows = None, []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if header is None:
            header = line.split(",")
            continue
        parts = line.split(",")
        if len(parts) != len(header):
            continue
        rows.append(parts)
    if header is None:
        raise SystemExit("no CSV header found")
    idx = {name: i for i, name in enumerate(header)}
    out = {}
    for c in COLS:
        if c in idx:
            out[c] = [float(r[idx[c]]) for r in rows]
    return out


def summarize(cols):
    lp = cols.get("lp__", [])
    nl = cols.get("n_leapfrog__", [])
    td = cols.get("treedepth__", [])
    ss = cols.get("stepsize__", [])
    dv = cols.get("divergent__", [])
    return {
        "draws": len(lp),
        "leapfrogs": int(sum(nl)),
        "depth_mean": statistics.fmean(td) if td else 0.0,
        "depth_max": max(td) if td else 0.0,
        "stepsize": ss[-1] if ss else 0.0,
        "divergent": sum(dv) / len(dv) if dv else 0.0,
        "lp_mean": statistics.fmean(lp) if lp else 0.0,
        "lp_sd": statistics.pstdev(lp) if len(lp) > 1 else 0.0,
    }


def rel(a, b):
    scale = max(abs(a), abs(b), 1e-12)
    return abs(a - b) / scale


def run_stanli(stan, data, args, run_bin):
    cmd = [str(run_bin), str(stan), str(data), "--seed", str(args.seed),
           "--warmup", str(args.warmup), "--samples", str(args.samples),
           "--sampler-stats"]
    depth = (args.stanli_max_depth if args.stanli_max_depth is not None
             else args.max_depth)
    if depth is not None:
        cmd += ["--max-depth", str(depth)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO,
                       timeout=args.timeout)
    if r.returncode != 0:
        raise RuntimeError(f"stanli_run failed: {r.stderr.strip()[:200]}")
    return read_csv(r.stdout, is_text=True)


def run_cmdstan(stan, data, args, cmdstan, tmp):
    """Build the model with CmdStan's make and sample once."""
    name = stan.stem
    work = tmp / f"cs_{name}"
    work.mkdir(parents=True, exist_ok=True)
    shutil.copy(stan, work / f"{name}.stan")
    exe = work / name
    r = subprocess.run(["make", str(exe.resolve())], cwd=cmdstan,
                       capture_output=True, text=True, timeout=1800)
    if r.returncode != 0:
        raise RuntimeError(f"cmdstan build failed: "
                           f"{r.stderr.strip()[-300:]}")
    out = work / "output.csv"
    # CmdStan's argument grammar: bare words are categories, leaves are
    # key=value. Mixing them up gets "is either mistyped or misplaced".
    depth = args.max_depth if args.max_depth is not None else 10
    cmd = [str(exe), "sample", f"num_warmup={args.warmup}",
           f"num_samples={args.samples}",
           "adapt", f"delta={args.delta}",
           "algorithm=hmc", "engine=nuts", f"max_depth={depth}",
           "data", f"file={data}", "random", f"seed={args.seed}",
           "output", f"file={out}"]
    r = subprocess.run(cmd, capture_output=True, text=True,
                       timeout=args.timeout)
    if not out.exists():
        raise RuntimeError(f"cmdstan sample failed: "
                           f"{r.stdout.strip()[-300:]}")
    return read_csv(out)


def compare(a, b, args):
    """a = stanli, b = cmdstan. Returns list of failure strings."""
    bad = []
    if rel(a["leapfrogs"], b["leapfrogs"]) > args.tol_leapfrog:
        bad.append(f"leapfrogs {a['leapfrogs']} vs {b['leapfrogs']} "
                   f"({rel(a['leapfrogs'], b['leapfrogs']):.2f} rel)")
    if rel(a["stepsize"], b["stepsize"]) > args.tol_stepsize:
        bad.append(f"stepsize {a['stepsize']:.4g} vs {b['stepsize']:.4g}")
    if a["depth_max"] > 0 and b["depth_max"] > 0:
        if abs(a["depth_max"] - b["depth_max"]) > 2:
            bad.append(f"max treedepth {a['depth_max']:.0f} vs "
                       f"{b['depth_max']:.0f}")
    sd = max(a["lp_sd"], b["lp_sd"], 1e-9)
    if abs(a["lp_mean"] - b["lp_mean"]) / sd > args.tol_lp:
        bad.append(f"mean lp__ {a['lp_mean']:.3f} vs {b['lp_mean']:.3f} "
                   f"({abs(a['lp_mean'] - b['lp_mean']) / sd:.2f} sd)")
    if abs(a["divergent"] - b["divergent"]) > 0.1:
        bad.append(f"divergence rate {a['divergent']:.3f} vs "
                   f"{b['divergent']:.3f}")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmdstan", type=pathlib.Path)
    ap.add_argument("pdb", type=pathlib.Path, nargs="?")
    ap.add_argument("models", nargs="*")
    ap.add_argument("--stan", type=pathlib.Path)
    ap.add_argument("--data", type=pathlib.Path)
    ap.add_argument("--run", type=pathlib.Path,
                    default=REPO / "build-rel" / "stanli_run")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--warmup", type=int, default=1000)
    ap.add_argument("--samples", type=int, default=1000)
    ap.add_argument("--delta", type=float, default=0.8)
    ap.add_argument("--max-depth", type=int, default=None)
    # Applies to stanli only: injects an asymmetry so the harness can be
    # tested against the bug class it exists for (a max tree depth that
    # silently differs from CmdStan's).
    ap.add_argument("--stanli-max-depth", type=int, default=None)
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--tol-leapfrog", type=float, default=0.5)
    ap.add_argument("--tol-stepsize", type=float, default=0.5)
    ap.add_argument("--tol-lp", type=float, default=0.5)
    args = ap.parse_args()

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_trace_"))
    jobs = []
    if args.stan:
        jobs.append((args.stan.stem, args.stan, args.data))
    else:
        pdb = args.pdb / "posterior_database"
        datas = {}
        for pj in sorted((pdb / "posteriors").glob("*.json")):
            meta = json.loads(pj.read_text())
            datas.setdefault(meta["model_name"], meta["data_name"])
        for m in args.models:
            stan = pdb / "models" / "stan" / f"{m}.stan"
            dz = pdb / "data" / "data" / f"{datas[m]}.json.zip"
            dj = tmp / f"{m}.json"
            with zipfile.ZipFile(dz) as z:
                dj.write_bytes(z.read(z.namelist()[0]))
            jobs.append((m, stan, dj))

    failures = 0
    for name, stan, data in jobs:
        try:
            s = summarize(run_stanli(stan, data, args, args.run))
            c = summarize(run_cmdstan(stan, data, args, args.cmdstan, tmp))
        except Exception as e:  # a build or run failure is a real failure
            print(f"ERROR {name}: {e}")
            failures += 1
            continue
        bad = compare(s, c, args)
        tag = "DIVERGED" if bad else "MATCH"
        print(f"{tag} {name}: leapfrog {s['leapfrogs']}/{c['leapfrogs']}, "
              f"stepsize {s['stepsize']:.4g}/{c['stepsize']:.4g}, "
              f"depth max {s['depth_max']:.0f}/{c['depth_max']:.0f}, "
              f"lp {s['lp_mean']:.2f}/{c['lp_mean']:.2f}")
        for b in bad:
            print(f"    {b}")
        failures += bool(bad)

    print(f"\n{len(jobs) - failures}/{len(jobs)} models agree at the "
          f"sampler level")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
