#!/usr/bin/env python3
"""Compare stanli's NUTS posteriors with posteriordb's reference posteriors.

For every posteriordb corpus model whose reference posterior has draws in
the given posteriordb checkout, stanli_run samples 4 chains with CmdStan's
default settings (1000 warmup and 1000 kept iterations, adapt delta 0.8,
max depth 10) and a fixed seed. For each parameter that both sides report,
the mean and the standard deviation are compared with

    z = (stanli - reference) / sqrt(mcse_stanli^2 + mcse_reference^2)

where each side's Monte Carlo standard error comes from its own draws by
batch means (mcse_mean below). A parameter fails when |z| > Z_LIMIT for
its mean or its standard deviation. Split R-hat of the stanli chains is
reported, and a value over RHAT_LIMIT is flagged but does not fail the
model on its own.

This is an on-demand check (TESTING.md), not part of PR CI. It needs no
third-party Python packages.

Usage: tools/verify_posteriors.py --references POSTERIORDB_WITH_DRAWS
           [--pdb deps/posteriordb] [--run build-rel/stanli_run]
           [--jobs N] [--out DIR] [--timeout S] [model ...]
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import pathlib
import statistics
import subprocess
import sys
import tempfile
import zipfile

from corpus_inventory import corpus_cases, materialize_data

REPO = pathlib.Path(__file__).resolve().parent.parent
Z_LIMIT = 4.0
RHAT_LIMIT = 1.01
SETTINGS = {"chains": 4, "warmup": 1000, "samples": 1000, "delta": 0.8,
            "max_depth": 10, "seed": 1234}
SAMPLER_COLUMNS = 7


def canonical_name(name):
    """stanli_run's `theta.1.2` as posteriordb's `theta[1,2]`."""
    base, *rest = name.split(".")
    if rest and all(part.isdigit() for part in rest):
        return f"{base}[{','.join(rest)}]"
    return name


def mcse_mean(chains):
    """(mean, Monte Carlo standard error of the mean) of a list of chains.

    Batch means: each chain is cut into batches of floor(sqrt(n)) draws
    (the last partial batch is dropped), and the variance of the batch
    means over all chains, divided by the number of batches, estimates
    the squared standard error. The estimate is consistent for a
    stationary chain whose autocorrelation dies out well within one
    batch. Chains of very different means inflate it, which R-hat
    reports separately.
    """
    draws = [x for chain in chains for x in chain]
    mean = statistics.fmean(draws)
    size = max(1, math.isqrt(min(len(c) for c in chains)))
    batches = [statistics.fmean(chain[i:i + size])
               for chain in chains
               for i in range(0, len(chain) - size + 1, size)]
    if len(batches) < 2:
        return mean, math.nan
    return mean, math.sqrt(statistics.variance(batches) / len(batches))


def mcse_sd(chains):
    """(standard deviation, its Monte Carlo standard error).

    The squared deviations from the pooled mean are batched like the draws
    in mcse_mean, and the delta method turns the error of the variance into
    one of the standard deviation: se(sd) = se(var) / (2 sd).
    """
    mean = statistics.fmean(x for chain in chains for x in chain)
    var, se_var = mcse_mean([[(x - mean) ** 2 for x in chain] for chain in chains])
    n = sum(len(c) for c in chains)
    sd = math.sqrt(var * n / (n - 1))
    return sd, (se_var / (2 * sd) if sd > 0 else math.nan)


def ess(chains):
    """Effective sample size implied by the batch-means error of the mean."""
    draws = [x for chain in chains for x in chain]
    _, se = mcse_mean(chains)
    var = statistics.variance(draws)
    return var / se ** 2 if se and se > 0 else math.nan


def split_rhat(chains):
    """Split R-hat (Gelman et al. 2013), without rank normalization."""
    halves = []
    for chain in chains:
        half = len(chain) // 2
        halves += [chain[:half], chain[len(chain) - half:]]
    n = min(len(h) for h in halves)
    if n < 2:
        return math.nan
    means = [statistics.fmean(h) for h in halves]
    within = statistics.fmean(statistics.variance(h) for h in halves)
    between = n * statistics.variance(means)
    if within == 0:
        return math.nan if between else 1.0
    return math.sqrt(((n - 1) / n * within + between / n) / within)


def z_score(a, se_a, b, se_b):
    if a == b:
        return 0.0
    se = math.hypot(se_a, se_b)
    if not math.isfinite(se):
        return math.nan
    if se == 0:
        return math.inf
    return (a - b) / se


def compare(stanli, reference):
    """Per-parameter rows for the parameters both sides have.

    stanli and reference map canonical names to lists of chains.
    """
    rows = []
    for name in sorted(set(stanli) & set(reference)):
        s, r = stanli[name], reference[name]
        sm, sm_se = mcse_mean(s)
        rm, rm_se = mcse_mean(r)
        ss, ss_se = mcse_sd(s)
        rs, rs_se = mcse_sd(r)
        row = {"name": name, "mean": sm, "ref_mean": rm,
               "z_mean": z_score(sm, sm_se, rm, rm_se),
               "sd": ss, "ref_sd": rs, "z_sd": z_score(ss, ss_se, rs, rs_se),
               "ess": ess(s), "ref_ess": ess(r), "rhat": split_rhat(s)}
        row["fail"] = any(not (abs(row[k]) <= Z_LIMIT) for k in ("z_mean", "z_sd"))
        rows.append(row)
    return rows


def verdict(rows):
    if not rows:
        return "NO_COMMON_PARAMETERS"
    return "FAIL" if any(r["fail"] for r in rows) else "PASS"


def parse_run(text, chains, samples):
    """{canonical name: [chain draws]} from stanli_run --sampler-stats output,
    whose rows come chain by chain."""
    lines = [line for line in text.splitlines() if line and not line.startswith("#")]
    header = lines[0].split(",")
    rows = [[float(x) for x in line.split(",")] for line in lines[1:]]
    if len(rows) != chains * samples:
        raise ValueError(f"expected {chains * samples} rows, got {len(rows)}")
    out = {}
    for j, name in enumerate(header[SAMPLER_COLUMNS:], SAMPLER_COLUMNS):
        out[canonical_name(name)] = [[rows[c * samples + i][j] for i in range(samples)]
                                     for c in range(chains)]
    divergent = sum(row[header.index("divergent__")] for row in rows)
    return out, int(divergent)


def load_reference(path):
    """{name: [chain draws]} from a posteriordb reference draws zip."""
    with zipfile.ZipFile(path) as z:
        chains = json.loads(z.read(z.namelist()[0]))
    return {canonical_name(name): [c[name] for c in chains] for name in chains[0]}


def reference_models(cases, draws_dir):
    """{model: reference draws path} for the posteriordb corpus models whose
    reference posterior has draws and uses the model's own dataset."""
    out = {}
    for name, case in cases.items():
        ref = case.metadata.get("reference_posterior_name")
        path = draws_dir / f"{ref}.json.zip"
        if (case.collection == "posteriordb" and ref and path.exists()
                and ref.split("-")[0] == case.metadata.get("data_name")):
            out[name] = path
    return out


def run_model(model, case, ref_path, run_bin, work, timeout):
    (work / model).mkdir()
    data = materialize_data(case, work / model)
    cmd = [str(run_bin), str(case.source), str(data),
           "--chains", str(SETTINGS["chains"]), "--warmup", str(SETTINGS["warmup"]),
           "--samples", str(SETTINGS["samples"]), "--delta", str(SETTINGS["delta"]),
           "--max-depth", str(SETTINGS["max_depth"]), "--seed", str(SETTINGS["seed"]),
           "--num-threads", "1", "--sampler-stats"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"model": model, "status": "TIMEOUT"}
    if proc.returncode != 0:
        err = (proc.stderr.strip().splitlines() or [f"exit {proc.returncode}"])[-1]
        return {"model": model, "status": "RUN_FAIL", "detail": err[:200]}
    try:
        draws, divergent = parse_run(proc.stdout, SETTINGS["chains"],
                                     SETTINGS["samples"])
    except (ValueError, IndexError) as exc:
        return {"model": model, "status": "RUN_FAIL", "detail": str(exc)[:200]}
    rows = compare(draws, load_reference(ref_path))
    return {"model": model, "status": verdict(rows), "divergent": divergent,
            "reference": ref_path.name, "rows": rows}


def summary_line(result):
    rows = result.get("rows", [])
    if not rows:
        return f"{result['status']:9s} {result['model']:40s} {result.get('detail', '')}"
    worst = max(rows, key=lambda r: max(abs(r["z_mean"]), abs(r["z_sd"]))
                if all(math.isfinite(r[k]) for k in ("z_mean", "z_sd")) else math.inf)
    rhat = max((r["rhat"] for r in rows if math.isfinite(r["rhat"])), default=math.nan)
    failed = sum(r["fail"] for r in rows)
    return (f"{result['status']:9s} {result['model']:40s} {len(rows):4d} params, "
            f"{failed} over |z| {Z_LIMIT:g}; worst {worst['name']} z_mean "
            f"{worst['z_mean']:+.1f} z_sd {worst['z_sd']:+.1f}; max R-hat {rhat:.3f}"
            + (" (over %g)" % RHAT_LIMIT if rhat > RHAT_LIMIT else "")
            + f"; {result['divergent']} divergent")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("models", nargs="*")
    ap.add_argument("--references", type=pathlib.Path, required=True,
                    help="posteriordb checkout that has reference_posteriors/draws")
    ap.add_argument("--pdb", type=pathlib.Path, default=REPO / "deps" / "posteriordb")
    ap.add_argument("--run", type=pathlib.Path, default=REPO / "build-rel" / "stanli_run")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=3600)
    ap.add_argument("--out", type=pathlib.Path,
                    help="write the per-parameter results as JSON here")
    args = ap.parse_args()
    draws_dir = (args.references / "posterior_database" / "reference_posteriors"
                 / "draws" / "draws")
    cases = corpus_cases(args.pdb / "posterior_database", include_language=True)
    available = reference_models(cases, draws_dir)
    if not available:
        ap.error(f"no reference draws for corpus models under {draws_dir}")
    models = args.models or sorted(available)
    unknown = [m for m in models if m not in available]
    if unknown:
        ap.error("no reference draws for: " + " ".join(unknown))
    print(f"{len(models)} models with reference draws; stanli_run {args.run}; "
          f"settings {SETTINGS}")
    with tempfile.TemporaryDirectory(prefix="stanli_posteriors_") as tmp:
        work = pathlib.Path(tmp)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            results = list(pool.map(
                lambda m: run_model(m, cases[m], available[m], args.run, work,
                                    args.timeout), models))
    for result in sorted(results, key=lambda r: (r["status"], r["model"])):
        print(summary_line(result))
    counts = {}
    for result in results:
        counts[result["status"]] = counts.get(result["status"], 0) + 1
    print("\n" + ", ".join(f"{n} {s}" for s, n in sorted(counts.items())))
    if args.out:
        args.out.write_text(json.dumps(results, indent=1, allow_nan=True) + "\n")
    return 0 if counts.get("PASS", 0) == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
