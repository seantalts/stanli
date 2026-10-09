#!/usr/bin/env python3
"""Do posteriors sampled in fast mode agree with default mode and with
reference posteriors?

Each selected corpus model is sampled by build-rel/stanli_run in default mode
and with --fast-math, with identical settings and seed. The arms are compared
by distribution, not by draw: trajectories differ as soon as any gradient
differs in its last bit.

  select   write the model sets (reference posteriors, collapse, largest
           fast-mode deviations) to a JSON file
  run      sample every model in both arms, keeping each CSV (gzip)
  analyze  summarise the CSVs, compare the arms and the references, flag
  report   write one CSV row per model from an analysis
  persist  count how often each flag recurs across rerun directories
  control  flag rates between runs of the same arm with different seeds

Metrics and the flag rules are documented in notes/performance/
2026-10-09-fast-mode-sampling-check.md and tested in
tests/test_fast_sampling_check.py.

Usage: python3 harnesses/fast_sampling_check.py select deps/posteriordb \
           --references <posteriordb with reference_posteriors> \
           --census census.json --verify-log verify_fast.tsv --out models.json
       python3 harnesses/fast_sampling_check.py run deps/posteriordb \
           --models models.json --out runs/main
       python3 harnesses/fast_sampling_check.py analyze deps/posteriordb \
           --references <same> --models models.json --out runs/main
"""
import argparse
import concurrent.futures
import csv
import gzip
import io
import json
import math
import multiprocessing
import pathlib
import subprocess
import sys
import time
import zipfile

import numpy as np
from scipy import stats

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

SAMPLER_COLUMNS = ["lp__", "accept_stat__", "stepsize__", "treedepth__",
                   "n_leapfrog__", "divergent__", "energy__"]
RHAT_LIMIT = 1.01
FAMILY_ALPHA = 0.01
RATE_Z = 3.0
RATE_MIN_DIFFERENCE = 0.005
METRICS = ("mean", "sd", "q05", "q95")
KINDS = ("mean", "sd", "q05", "q95", "rhat", "divergent", "treedepth", "stay")


def canonical_name(name):
    base, *rest = name.split(".")
    if rest and all(part.isdigit() for part in rest):
        return f"{base}[{','.join(rest)}]"
    return name


def z_critical(n_tests, alpha):
    return float(stats.norm.ppf(1.0 - alpha / (2.0 * n_tests)))


def split_chains(x):
    n = x.shape[0]
    if n == 1:
        return x
    half = n / 2.0
    return np.concatenate([x[:int(math.floor(half))],
                           x[int(math.ceil(half + 1)) - 1:]], axis=1)


def z_scale(x):
    ranks = stats.rankdata(x, method="average").reshape(x.shape)
    size = x.size
    return stats.norm.ppf((ranks - 0.375) / (size - 0.75 + 1.0))


def _returns_nan(x):
    return (not np.all(np.isfinite(x))) or np.ptp(x) <= np.finfo(float).eps


def _autocovariance(x):
    n = x.shape[0]
    centred = x - x.mean(axis=0)
    size = 1 << (2 * n - 1).bit_length()
    spectrum = np.fft.rfft(centred, n=size, axis=0)
    ac = np.fft.irfft(np.abs(spectrum) ** 2, n=size, axis=0)[:n]
    var = x.var(axis=0, ddof=1)
    with np.errstate(divide="ignore", invalid="ignore"):
        out = ac / ac[0] * var * (n - 1) / n
    out[:, var == 0] = 0.0
    return out


def _ess(x):
    """Effective sample size of a (draws, chains) matrix, after
    Vehtari et al. 2021 as in the posterior package."""
    nchains = x.shape[1]
    n = x.shape[0]
    if n < 3 or _returns_nan(x):
        return math.nan
    acov = _autocovariance(x)
    means = np.concatenate([[math.nan], acov.mean(axis=1)])
    mean_var = means[1] * n / (n - 1)
    var_plus = mean_var * (n - 1) / n
    if nchains > 1:
        var_plus += x.mean(axis=0).var(ddof=1)
    rho = np.zeros(n + 2)
    t = 0
    even = 1.0
    rho[t + 1] = even
    odd = 1.0 - (mean_var - means[t + 2]) / var_plus
    rho[t + 2] = odd
    while t < n - 5 and not math.isnan(even + odd) and (even + odd) > 0:
        t += 2
        even = 1.0 - (mean_var - means[t + 1]) / var_plus
        odd = 1.0 - (mean_var - means[t + 2]) / var_plus
        if even + odd >= 0:
            rho[t + 1] = even
            rho[t + 2] = odd
    max_t = t
    if even > 0:
        rho[max_t + 1] = even
    t = 0
    while t <= max_t - 4:
        t += 2
        if rho[t + 1] + rho[t + 2] > rho[t - 1] + rho[t]:
            rho[t + 1] = (rho[t - 1] + rho[t]) / 2.0
            rho[t + 2] = rho[t + 1]
    size = nchains * n
    tau = -1.0 + 2.0 * rho[1:max_t + 1].sum() + rho[max_t + 1]
    tau = max(tau, 1.0 / math.log10(size))
    return size / tau


def _rhat(x):
    if _returns_nan(x):
        return math.nan
    n = x.shape[0]
    chain_mean = x.mean(axis=0)
    between = n * chain_mean.var(ddof=1)
    within = x.var(axis=0, ddof=1).mean()
    return math.sqrt((between / within + n - 1) / n)


def _ess_quantile(x, prob):
    if _returns_nan(x):
        return math.nan
    indicator = (x <= np.quantile(x, prob)).astype(float)
    return _ess(split_chains(indicator))


def _mcse_quantile(x, prob, ess):
    if not math.isfinite(ess):
        return math.nan
    p = np.array([0.1586553, 0.8413447])
    a = stats.beta.ppf(p, ess * prob + 1, ess * (1 - prob) + 1)
    sims = np.sort(x.ravel())
    size = sims.size
    lo = sims[max(int(math.floor(a[0] * size)), 1) - 1]
    hi = sims[min(int(math.ceil(a[1] * size)), size) - 1]
    return (hi - lo) / 2.0


def posterior_summary(draws):
    """Summary of one column. `draws` is (chains, iterations)."""
    x = np.asarray(draws, dtype=float).T
    out = {k: math.nan for k in (
        "rhat", "ess_bulk", "ess_tail", "ess_mean", "mcse_mean", "mcse_sd",
        "mcse_q05", "mcse_q95")}
    out["mean"] = float(x.mean())
    out["sd"] = float(x.std(ddof=1))
    out["q05"] = float(np.quantile(x, 0.05))
    out["q95"] = float(np.quantile(x, 0.95))
    if not np.all(np.isfinite(x)) or out["sd"] == 0.0:
        return out
    split = split_chains(x)
    folded = np.abs(x - np.median(x))
    out["rhat"] = max(_rhat(z_scale(split)), _rhat(z_scale(split_chains(folded))))
    out["ess_bulk"] = _ess(z_scale(split))
    q05_ess = _ess_quantile(x, 0.05)
    q95_ess = _ess_quantile(x, 0.95)
    out["ess_tail"] = min(q05_ess, q95_ess)
    out["ess_mean"] = _ess(split)
    out["mcse_mean"] = out["sd"] / math.sqrt(out["ess_mean"])
    centred = x - x.mean()
    ess_sq = _ess(split_chains(centred ** 2))
    var = float(np.mean(centred ** 2))
    varvar = (float(np.mean(centred ** 4)) - var ** 2) / ess_sq
    out["mcse_sd"] = math.sqrt(varvar / var / 4.0)
    out["mcse_q05"] = _mcse_quantile(x, 0.05, q05_ess)
    out["mcse_q95"] = _mcse_quantile(x, 0.95, q95_ess)
    return out


def stay_count(chains):
    """Transitions that returned the previous state, over arrays shaped
    (chains, draws) taken together as one parameter vector."""
    stacked = np.stack(chains, axis=-1)
    return int(np.all(stacked[:, 1:] == stacked[:, :-1], axis=-1).sum())


def parse_run_csv(source, chains, warmup, samples, max_depth=10):
    """Split stanli_run --sampler-stats --save-warmup output.

    Rows come chain by chain, each with its warmup rows first."""
    lines = io.StringIO(source) if isinstance(source, str) else source
    header = next(lines).strip().split(",")
    if header[:len(SAMPLER_COLUMNS)] != SAMPLER_COLUMNS:
        raise ValueError("missing sampler columns")
    rows = np.loadtxt(lines, delimiter=",", ndmin=2)
    per_chain = warmup + samples
    if rows.shape[0] != chains * per_chain:
        raise ValueError(f"expected {chains * per_chain} rows, got {rows.shape[0]}")
    cube = rows.reshape(chains, per_chain, -1)
    kept = cube[:, warmup:, :]
    leap = header.index("n_leapfrog__")
    names = [canonical_name(h) for h in header[len(SAMPLER_COLUMNS):]]
    columns = {name: kept[:, :, len(SAMPLER_COLUMNS) + i]
               for i, name in enumerate(names)}
    run = {
        "draws": chains * samples,
        "gradients": int(kept[:, :, leap].sum()),
        "warmup_gradients": int(cube[:, :warmup, leap].sum()),
        "divergent": int(kept[:, :, header.index("divergent__")].sum()),
        "max_treedepth": int((kept[:, :, header.index("treedepth__")]
                              >= max_depth).sum()),
        "accept_stat": float(kept[:, :, header.index("accept_stat__")].mean()),
        "stepsize": float(kept[:, :, header.index("stepsize__")].mean()),
        "chains": chains,
        "lp": kept[:, :, 0],
        "columns": columns,
    }
    run["stay"] = stay_count(list(columns.values())) if columns else 0
    return run


def arm_record(run):
    """The stored form of a sampled arm: summaries instead of draws."""
    summary = {name: posterior_summary(col) for name, col in run["columns"].items()}
    record = {k: run[k] for k in (
        "draws", "gradients", "warmup_gradients", "divergent", "max_treedepth",
        "accept_stat", "stepsize", "stay", "chains")}
    record["summary"] = summary
    record["lp"] = posterior_summary(run["lp"])
    record["n_params"] = len(summary)
    return record


def reference_columns(chain_dicts):
    """posteriordb reference draws: a list of {name: [draws]} per chain."""
    names = chain_dicts[0].keys()
    return {canonical_name(n): np.array([c[n] for c in chain_dicts], dtype=float)
            for n in names}


def reference_arm(columns):
    summary = {name: posterior_summary(col) for name, col in columns.items()}
    some = next(iter(columns.values()))
    return {"summary": summary, "draws": some.size, "n_params": len(summary)}


def _usable(s):
    return all(math.isfinite(s[k]) for k in ("mean", "sd", "mcse_mean", "mcse_sd"))


def _column_scores(a, b):
    se_mean = math.hypot(a["mcse_mean"], b["mcse_mean"])
    z_mean = (a["mean"] - b["mean"]) / se_mean if se_mean > 0 else 0.0
    sd = math.hypot(a["mcse_sd"] / a["sd"], b["mcse_sd"] / b["sd"])
    log_ratio = math.log(a["sd"] / b["sd"])
    z_sd = log_ratio / sd if sd > 0 else 0.0
    out = {"z_mean": z_mean, "z_sd": z_sd, "sd_ratio": a["sd"] / b["sd"],
           "mean_diff_sd": (a["mean"] - b["mean"]) / max(a["sd"], b["sd"])}
    for q in ("q05", "q95"):
        se = math.hypot(a["mcse_" + q], b["mcse_" + q])
        out["z_" + q] = (a[q] - b[q]) / se if se > 0 and math.isfinite(se) else 0.0
    return out


def _rate_flag(kind, count_a, n_a, count_b, n_b):
    p_a, p_b = count_a / n_a, count_b / n_b
    pooled = (count_a + count_b) / (n_a + n_b)
    se = math.sqrt(pooled * (1 - pooled) * (1 / n_a + 1 / n_b))
    z = (p_a - p_b) / se if se > 0 else 0.0
    if abs(p_a - p_b) >= RATE_MIN_DIFFERENCE and abs(z) > RATE_Z:
        return {"kind": kind, "a": p_a, "b": p_b, "z": z}
    return None


def _efficiency(arm):
    values = [s["ess_bulk"] for s in arm["summary"].values()
              if math.isfinite(s["ess_bulk"])]
    tails = [s["ess_tail"] for s in arm["summary"].values()
             if math.isfinite(s["ess_tail"])]
    if not values:
        return None
    grads = arm["gradients"]
    return {"min_bulk": min(values) / grads, "median_bulk": float(np.median(values)) / grads,
            "min_tail": min(tails) / grads if tails else math.nan,
            "min_bulk_ess": min(values), "min_tail_ess": min(tails) if tails else math.nan}


def compare_arms(a, b, compare_sampler=True, alpha=FAMILY_ALPHA):
    """Compare two arms' posteriors. z = (a - b) over the combined MCSE."""
    shared = [n for n in a["summary"] if n in b["summary"]
              and _usable(a["summary"][n]) and _usable(b["summary"][n])
              and a["summary"][n]["sd"] > 0 and b["summary"][n]["sd"] > 0]
    scores = {n: _column_scores(a["summary"][n], b["summary"][n]) for n in shared}
    n_tests = max(len(METRICS) * len(shared), 1)
    zc = z_critical(n_tests, alpha)
    result = {"n_columns": len(shared), "n_tests": n_tests, "z_critical": zc,
              "worst": {}, "flags": []}
    for metric, key in (("mean", "z_mean"), ("sd", "z_sd"), ("q05", "z_q05"),
                        ("q95", "z_q95")):
        if not shared:
            continue
        name = max(shared, key=lambda n: abs(scores[n][key]))
        worst = {"column": name, "z": scores[name][key]}
        if metric == "mean":
            worst["diff_sd"] = scores[name]["mean_diff_sd"]
        if metric == "sd":
            worst["ratio"] = scores[name]["sd_ratio"]
        result["worst"][metric] = worst
        if abs(worst["z"]) > zc:
            count = sum(1 for n in shared if abs(scores[n][key]) > zc)
            result["flags"].append({"kind": metric, "column": name,
                                    "z": worst["z"], "columns_over": count})
    result["max_abs_mean_diff_sd"] = max(
        (abs(s["mean_diff_sd"]) for s in scores.values()), default=0.0)
    result["scores"] = scores
    rhat_a = [a["summary"][n]["rhat"] for n in shared
              if math.isfinite(a["summary"][n]["rhat"])]
    rhat_b = [b["summary"][n]["rhat"] for n in shared
              if math.isfinite(b["summary"][n]["rhat"])]
    result["rhat_max"] = {"a": max(rhat_a, default=math.nan),
                          "b": max(rhat_b, default=math.nan)}
    one_arm = (result["rhat_max"]["a"] > RHAT_LIMIT) != (result["rhat_max"]["b"] > RHAT_LIMIT)
    if compare_sampler and one_arm:
        result["flags"].append({"kind": "rhat", **result["rhat_max"]})
    if compare_sampler:
        ea, eb = _efficiency(a), _efficiency(b)
        result["efficiency"] = {"a": ea, "b": eb}
        if ea and eb:
            result["ess_per_gradient_ratio"] = ea["min_bulk"] / eb["min_bulk"]
            result["ess_per_gradient_ratio_median"] = (
                ea["median_bulk"] / eb["median_bulk"])
            if math.isfinite(ea["min_tail"]) and math.isfinite(eb["min_tail"]):
                result["ess_tail_per_gradient_ratio"] = ea["min_tail"] / eb["min_tail"]
        result["gradient_ratio"] = a["gradients"] / b["gradients"]
        for kind, key in (("divergent", "divergent"), ("treedepth", "max_treedepth"),
                          ("stay", "stay")):
            flag = _rate_flag(kind, a[key], a["draws"], b[key], b["draws"])
            result["rates_" + kind] = {"a": a[key] / a["draws"], "b": b[key] / b["draws"]}
            if flag:
                result["flags"].append(flag)
    return result


def _flag_sign(flag):
    if "z" in flag:
        return 1 if flag["z"] > 0 else -1
    return 1 if flag["a"] > flag["b"] else -1


def persistence(comparisons):
    """For each flag kind, the number of comparisons that raised it and how
    many of those agree on direction."""
    out = {}
    for comparison in comparisons:
        seen = set()
        for flag in comparison["flags"]:
            if flag["kind"] in seen:
                continue
            seen.add(flag["kind"])
            entry = out.setdefault(flag["kind"], {"pos": 0, "neg": 0})
            entry["pos" if _flag_sign(flag) > 0 else "neg"] += 1
    return {kind: {"flagged": e["pos"] + e["neg"], "of": len(comparisons),
                   "same_direction": max(e["pos"], e["neg"])}
            for kind, e in out.items()}


def available_references(ref_root):
    root = pathlib.Path(ref_root) / "posterior_database" / "reference_posteriors" / "draws" / "draws"
    return {p.name[:-len(".json.zip")]: p for p in root.glob("*.json.zip")}


def load_reference(path):
    with zipfile.ZipFile(path) as z:
        chains = json.loads(z.read(z.namelist()[0]))
    return reference_columns(chains)


def reference_models(cases, refs):
    """{model: reference posterior name} for corpus posteriordb cases whose
    reference draws exist and belong to the same dataset."""
    out = {}
    for name, case in cases.items():
        meta = case.metadata
        ref = meta.get("reference_posterior_name") if case.collection == "posteriordb" else None
        if ref and ref in refs and ref.split("-")[0] == meta["data_name"]:
            out[name] = ref
    return out


def deviation_ranking(log_text):
    rows = []
    for line in log_text.splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            try:
                rows.append((parts[0], float(parts[1])))
            except ValueError:
                continue
    return sorted(rows, key=lambda r: -r[1])


def collapsing_models(census):
    out = []
    for model, entry in census.items():
        if any("opcode" in t and not t["refusal"] for t in entry["rows"]):
            out.append(model)
    return sorted(out)


def cmd_select(args):
    from corpus_inventory import corpus_cases
    cases = corpus_cases(args.pdb)
    refs = available_references(args.references)
    ref_models = reference_models(cases, refs)
    collapse = collapsing_models(json.loads(pathlib.Path(args.census).read_text()))
    ranking = deviation_ranking(pathlib.Path(args.verify_log).read_text())
    top = [m for m, rel in ranking if rel > 0][:args.top_deviation]
    sets = {
        "reference": sorted(ref_models),
        "reference_posterior": ref_models,
        "collapse": [m for m in collapse if m in cases],
        "deviation": [m for m in top if m in cases],
    }
    sets["extra"] = sorted((set(sets["collapse"]) | set(sets["deviation"]))
                           - set(sets["reference"]))
    sets["all"] = sorted(set(sets["reference"]) | set(sets["extra"]))
    pathlib.Path(args.out).write_text(json.dumps(sets, indent=1, sort_keys=True))
    for key in ("reference", "collapse", "deviation", "extra", "all"):
        print(f"{key}: {len(sets[key])}")


def stanli_run_command(args, case, data, arm, seed):
    cmd = [str(args.run_bin), str(case.source), str(data), "--chains", str(args.chains),
           "--warmup", str(args.warmup), "--samples", str(args.samples),
           "--delta", str(args.delta), "--seed", str(seed), "--sampler-stats",
           "--save-warmup", "--num-threads", str(args.threads)]
    if arm == "fast":
        cmd.append("--fast-math")
    return cmd


def run_one(args, case, data, arm, seed, out):
    target = out / case.name / f"{arm}.csv.gz"
    status_file = out / case.name / f"{arm}.status.json"
    if target.exists() and status_file.exists() and not args.force:
        return case.name, arm, json.loads(status_file.read_text())["status"]
    target.parent.mkdir(parents=True, exist_ok=True)
    cmd = stanli_run_command(args, case, data, arm, seed)
    start = time.monotonic()
    status = "ok"
    try:
        with gzip.open(target, "wb", compresslevel=3) as sink:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                for chunk in iter(lambda: proc.stdout.read(1 << 20), b""):
                    sink.write(chunk)
                    if time.monotonic() - start > args.timeout:
                        proc.kill()
                        status = "timeout"
                        break
                proc.wait(timeout=60)
            finally:
                if proc.poll() is None:
                    proc.kill()
            err = proc.stderr.read().decode(errors="replace")
        if status == "ok" and proc.returncode != 0:
            status = f"exit {proc.returncode}"
    except Exception as exc:
        status, err = f"error {exc}", ""
    (out / case.name / f"{arm}.stderr").write_text(err[-20000:])
    status_file.write_text(json.dumps({"status": status, "seed": seed, "cmd": cmd}))
    return case.name, arm, status


def selected_models(args):
    sets = json.loads(pathlib.Path(args.models).read_text())
    chosen = args.only.split(",") if args.only else sets[args.set]
    return sets, chosen


def cmd_run(args):
    from corpus_inventory import corpus_cases, materialize_data
    cases = corpus_cases(args.pdb)
    sets, chosen = selected_models(args)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    data_dir = out / "_data"
    data_dir.mkdir(exist_ok=True)
    jobs = []
    for model in chosen:
        data = materialize_data(cases[model], data_dir)
        for arm in ("default", "fast"):
            jobs.append((cases[model], data, arm))
    jobs.sort(key=lambda j: j[0].name)
    done = 0
    with concurrent.futures.ThreadPoolExecutor(args.parallel) as pool:
        futs = [pool.submit(run_one, args, c, d, arm, args.seed, out) for c, d, arm in jobs]
        for fut in concurrent.futures.as_completed(futs):
            name, arm, status = fut.result()
            done += 1
            print(f"[{done}/{len(jobs)}] {name} {arm} {status}", flush=True)


def summarize_arm(job):
    path, chains, warmup, samples = job
    try:
        with gzip.open(path, "rt") as f:
            run = parse_run_csv(f, chains, warmup, samples)
        return arm_record(run)
    except Exception as exc:
        return {"error": str(exc)}


def analysis_for(model, arms, reference, status):
    entry = {"model": model, "status": status}
    d, f = arms.get("default"), arms.get("fast")
    if not d or not f or "error" in d or "error" in f:
        entry["error"] = {"default": (d or {}).get("error", status.get("default")),
                          "fast": (f or {}).get("error", status.get("fast"))}
        return entry
    entry["fast_vs_default"] = compare_arms(f, d)
    entry["arms"] = {k: {kk: vv for kk, vv in arm.items() if kk != "summary"}
                     for k, arm in (("default", d), ("fast", f))}
    if reference is not None:
        entry["fast_vs_reference"] = compare_arms(f, reference, compare_sampler=False)
        entry["default_vs_reference"] = compare_arms(d, reference, compare_sampler=False)
    return entry


def cmd_analyze(args):
    from corpus_inventory import corpus_cases
    sets, chosen = selected_models(args)
    out = pathlib.Path(args.out)
    refs = available_references(args.references)
    jobs, index = [], []
    for model in chosen:
        for arm in ("default", "fast"):
            path = out / model / f"{arm}.csv.gz"
            summary = out / model / f"{arm}.summary.json"
            if summary.exists() and not args.force:
                continue
            if path.exists():
                jobs.append((path, args.chains, args.warmup, args.samples))
                index.append((model, arm))
    print(f"summarising {len(jobs)} arms", flush=True)
    with multiprocessing.Pool(args.processes) as pool:
        for (model, arm), record in zip(index, pool.imap(summarize_arm, jobs)):
            (out / model / f"{arm}.summary.json").write_text(json.dumps(record))
    results = {}
    reference_cache = {}
    for model in chosen:
        arms, status = {}, {}
        for arm in ("default", "fast"):
            s = out / model / f"{arm}.summary.json"
            st = out / model / f"{arm}.status.json"
            status[arm] = json.loads(st.read_text())["status"] if st.exists() else "missing"
            arms[arm] = json.loads(s.read_text()) if s.exists() else None
        reference = None
        ref_name = sets.get("reference_posterior", {}).get(model)
        if ref_name and ref_name in refs:
            if ref_name not in reference_cache:
                reference_cache[ref_name] = reference_arm(load_reference(refs[ref_name]))
            reference = reference_cache[ref_name]
        results[model] = analysis_for(model, arms, reference, status)
    summary_path = out / "analysis.json"
    summary_path.write_text(json.dumps(results, allow_nan=True))
    print(f"wrote {summary_path}")


def fmt(x, digits=2):
    if x is None or (isinstance(x, float) and not math.isfinite(x)):
        return "-"
    return f"{x:.{digits}f}"


def cmd_persist(args):
    runs = [json.loads((pathlib.Path(d) / "analysis.json").read_text()) for d in args.dirs]
    models = sorted(set.intersection(*(set(r) for r in runs)))
    print("| model | flag | seeds flagged | same direction | seeds |")
    print("| --- | --- | ---: | ---: | ---: |")
    for model in models:
        comps = [r[model][args.key] for r in runs if args.key in r[model]]
        found = persistence(comps)
        if not found:
            print(f"| {model} | none | 0 | - | {len(comps)} |")
        for kind, e in sorted(found.items()):
            print(f"| {model} | {kind} | {e['flagged']} | {e['same_direction']} | {e['of']} |")


def cmd_control(args):
    dirs = [pathlib.Path(d) for d in args.dirs]
    models = args.only.split(",")
    print("| model | pairing | pairs | any flag | " + " | ".join(KINDS) + " |")
    print("| --- | --- | ---: | ---: | " + " | ".join("---:" for _ in KINDS) + " |")
    for model in models:
        arms = {}
        for i, d in enumerate(dirs):
            for arm in ("default", "fast"):
                path = d / model / f"{arm}.summary.json"
                if path.exists():
                    arms[(i, arm)] = json.loads(path.read_text())
        pairings = {
            "default vs default": [((i, "default"), (j, "default"))
                                   for i in range(len(dirs)) for j in range(i)],
            "fast vs fast": [((i, "fast"), (j, "fast"))
                             for i in range(len(dirs)) for j in range(i)],
            "fast vs default": [((i, "fast"), (j, "default"))
                                for i in range(len(dirs)) for j in range(len(dirs))],
        }
        for name, pairs in pairings.items():
            comps = [compare_arms(arms[a], arms[b]) for a, b in pairs
                     if a in arms and b in arms
                     and "error" not in arms[a] and "error" not in arms[b]]
            if not comps:
                continue
            counts = {k: sum(any(f["kind"] == k for f in c["flags"]) for c in comps)
                      for k in KINDS}
            anyflag = sum(bool(c["flags"]) for c in comps)
            print(f"| {model} | {name} | {len(comps)} | {anyflag} | "
                  + " | ".join(str(counts[k]) for k in KINDS) + " |")


def report_rows(results, sets=None):
    sets = sets or {}
    rows = []
    for model, e in sorted(results.items()):
        row = {"model": model,
               "reference": int(model in sets.get("reference", [])),
               "collapse": int(model in sets.get("collapse", [])),
               "deviation": int(model in sets.get("deviation", []))}
        c = e.get("fast_vs_default")
        if not c:
            row["note"] = f"not compared: {e.get('error')}"
            rows.append(row)
            continue
        arms = e["arms"]
        row.update({
            "columns": c["n_columns"],
            "z_mean": c["worst"]["mean"]["z"], "z_sd": c["worst"]["sd"]["z"],
            "z_q05": c["worst"]["q05"]["z"], "z_q95": c["worst"]["q95"]["z"],
            "rhat_max_fast": c["rhat_max"]["a"], "rhat_max_default": c["rhat_max"]["b"],
            "ess_per_gradient_ratio": c.get("ess_per_gradient_ratio"),
            "ess_per_gradient_ratio_median": c.get("ess_per_gradient_ratio_median"),
            "gradient_ratio": c.get("gradient_ratio"),
            "divergent_fast": arms["fast"]["divergent"],
            "divergent_default": arms["default"]["divergent"],
            "max_treedepth_fast": arms["fast"]["max_treedepth"],
            "max_treedepth_default": arms["default"]["max_treedepth"],
            "stay_fast": arms["fast"]["stay"], "stay_default": arms["default"]["stay"],
            "flags": " ".join(sorted({f["kind"] for f in c["flags"]})),
        })
        for key in ("fast_vs_reference", "default_vs_reference"):
            if key in e:
                row[key + "_flags"] = " ".join(sorted({f["kind"] for f in e[key]["flags"]}))
                row[key + "_max_mean_diff_sd"] = e[key]["max_abs_mean_diff_sd"]
        rows.append(row)
    return rows


def cmd_report(args):
    results = json.loads((pathlib.Path(args.out) / "analysis.json").read_text())
    sets = json.loads(pathlib.Path(args.models).read_text()) if args.models else None
    rows = report_rows(results, sets)
    columns = []
    for row in rows:
        columns += [k for k in row if k not in columns]
    target = open(args.csv, "w", newline="") if args.csv else sys.stdout
    writer = csv.DictWriter(target, columns, restval="", extrasaction="ignore")
    writer.writeheader()
    for row in rows:
        writer.writerow({k: (f"{v:.6g}" if isinstance(v, float) else v)
                         for k, v in row.items()})


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("pdb", type=pathlib.Path)
        p.add_argument("--references", type=pathlib.Path,
                       help="posteriordb checkout that has reference_posteriors/")
        p.add_argument("--models", type=pathlib.Path)
        p.add_argument("--set", default="all")
        p.add_argument("--only", default="")
        p.add_argument("--out", type=pathlib.Path, required=True)
        p.add_argument("--chains", type=int, default=4)
        p.add_argument("--warmup", type=int, default=1000)
        p.add_argument("--samples", type=int, default=1000)
        p.add_argument("--force", action="store_true")

    p = sub.add_parser("select")
    p.add_argument("pdb", type=pathlib.Path)
    p.add_argument("--references", type=pathlib.Path, required=True)
    p.add_argument("--census", required=True)
    p.add_argument("--verify-log", required=True)
    p.add_argument("--top-deviation", type=int, default=30)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_select)

    p = sub.add_parser("run")
    common(p)
    p.add_argument("--run-bin", type=pathlib.Path, default=REPO / "build-rel" / "stanli_run")
    p.add_argument("--seed", type=int, default=20261009)
    p.add_argument("--delta", type=float, default=0.8)
    p.add_argument("--parallel", type=int, default=8)
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--timeout", type=float, default=1800)
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("analyze")
    common(p)
    p.add_argument("--processes", type=int, default=8)
    p.set_defaults(func=cmd_analyze)

    p = sub.add_parser("persist")
    p.add_argument("dirs", nargs="+")
    p.add_argument("--key", default="fast_vs_default",
                   choices=["fast_vs_default", "fast_vs_reference", "default_vs_reference"])
    p.set_defaults(func=cmd_persist)

    p = sub.add_parser("control")
    p.add_argument("dirs", nargs="+")
    p.add_argument("--only", required=True)
    p.set_defaults(func=cmd_control)

    p = sub.add_parser("report")
    p.add_argument("--out", type=pathlib.Path, required=True)
    p.add_argument("--models", type=pathlib.Path)
    p.add_argument("--csv")
    p.set_defaults(func=cmd_report)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
