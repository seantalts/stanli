#!/usr/bin/env python3
"""Generate docs/internal/artifacts/benchmark-points.json.gz.

For every corpus model, run Stanli NUTS on the benchmark data with one chain,
a fixed seed, 1000 warmup and 1000 sampling iterations and the default
adaptation, saving warmup. Ten iterations are chosen at cumulative-leapfrog
quantiles over the whole run, and their unconstrained draws become the model's
evaluation points. The run's gradient count, divergences and max-treedepth hits
are recorded with them. A model whose run fails, times out or yields a
non-finite point is recorded with the reason and keeps the fixed points. Each
model runs in its own process.

    python3 tools/gen_benchmark_points.py --lib build-points/libstanli.dylib
"""

import argparse
import concurrent.futures
import ctypes
import json
import math
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import benchmark_points as bp

REPO = pathlib.Path(__file__).resolve().parent.parent
PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
POINTS = 10
SAMPLER_COLS = 7
STAT_TREEDEPTH, STAT_LEAPFROG, STAT_DIVERGENT = 3, 4, 5
MAX_DEPTH = 10
DELTA = 0.8
ERR_LEN = 4096
MAX_VALUE_DOUBLES = 1 << 26


class _ModelOpts(ctypes.Structure):
    _fields_ = [("seed", ctypes.c_uint32), ("threads_per_chain", ctypes.c_int),
                ("fast_math", ctypes.c_int)]


class _SampleOpts(ctypes.Structure):
    _fields_ = [("seed", ctypes.c_uint32), ("chains", ctypes.c_int), ("chain_id", ctypes.c_int),
                ("warmup", ctypes.c_int), ("samples", ctypes.c_int), ("thin", ctypes.c_int),
                ("delta", ctypes.c_double), ("max_depth", ctypes.c_int), ("save_warmup", ctypes.c_int),
                ("init_radius", ctypes.c_double), ("inits", ctypes.POINTER(ctypes.c_double)),
                ("num_threads", ctypes.c_int)]


class _MetricOpts(ctypes.Structure):
    _fields_ = [("metric", ctypes.c_int), ("inv_metric", ctypes.POINTER(ctypes.c_double)),
                ("step_size", ctypes.c_double), ("adapt_engaged", ctypes.c_int),
                ("adapted_step_size", ctypes.POINTER(ctypes.c_double)),
                ("adapted_inv_metric", ctypes.POINTER(ctypes.c_double))]


class _SampleReport(ctypes.Structure):
    _fields_ = [("warmup_seconds", ctypes.c_double), ("sampling_seconds", ctypes.c_double),
                ("n_divergent", ctypes.c_int64), ("n_max_treedepth", ctypes.c_int64)]


_PROGRESS = ctypes.CFUNCTYPE(None, ctypes.c_int32, ctypes.c_int64, ctypes.c_int64, ctypes.c_int32,
                             ctypes.c_void_p)
_POLL = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
_DP = ctypes.POINTER(ctypes.c_double)


def _bind(path):
    lib = ctypes.CDLL(str(path))
    lib.stanli_model_new_with_opts.restype = ctypes.c_void_p
    lib.stanli_model_new_with_opts.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(_ModelOpts),
                                               ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_model_free.argtypes = [ctypes.c_void_p]
    lib.stanli_n_unconstrained.restype = ctypes.c_int64
    lib.stanli_n_unconstrained.argtypes = [ctypes.c_void_p]
    lib.stanli_n_constrained.restype = ctypes.c_int64
    lib.stanli_n_constrained.argtypes = [ctypes.c_void_p]
    lib.stanli_wa_n_columns.restype = ctypes.c_int64
    lib.stanli_wa_n_columns.argtypes = [ctypes.c_void_p]
    lib.stanli_grad.restype = ctypes.c_int
    lib.stanli_grad.argtypes = [ctypes.c_void_p, _DP, _DP, _DP]
    lib.stanli_sample_opts_init.argtypes = [ctypes.POINTER(_SampleOpts)]
    lib.stanli_n_stored_draws.restype = ctypes.c_int64
    lib.stanli_n_stored_draws.argtypes = [ctypes.POINTER(_SampleOpts)]
    lib.stanli_metric_opts_init.argtypes = [ctypes.POINTER(_MetricOpts)]
    lib.stanli_sample_multi_metric.restype = ctypes.c_int
    lib.stanli_sample_multi_metric.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(_SampleOpts), ctypes.POINTER(_MetricOpts), ctypes.c_int,
        _DP, _DP, _DP, _PROGRESS, ctypes.c_void_p, _POLL, ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(_SampleReport), ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_build_id.restype = ctypes.c_char_p
    lib.stanli_abi_version.restype = ctypes.c_int
    return lib


def _model(lib, mir, data, seed):
    opts = _ModelOpts()
    opts.seed, opts.threads_per_chain, opts.fast_math = seed, 1, 0
    err = ctypes.create_string_buffer(ERR_LEN)
    handle = lib.stanli_model_new_with_opts(pathlib.Path(mir).read_text().encode(),
                                            pathlib.Path(data).read_bytes(), ctypes.byref(opts),
                                            err, ERR_LEN)
    if not handle:
        raise RuntimeError("model construction failed: " + err.value.decode(errors="replace"))
    return handle


def _sample(lib, handle, seed, warmup, draws):
    n = lib.stanli_n_unconstrained(handle)
    opts = _SampleOpts()
    lib.stanli_sample_opts_init(ctypes.byref(opts))
    opts.seed, opts.chains, opts.chain_id, opts.num_threads = seed, 1, 1, 1
    opts.warmup, opts.samples, opts.thin = warmup, draws, 1
    opts.delta, opts.max_depth, opts.save_warmup = DELTA, MAX_DEPTH, 1
    metric = _MetricOpts()
    lib.stanli_metric_opts_init(ctypes.byref(metric))
    rows = lib.stanli_n_stored_draws(ctypes.byref(opts))
    chain = (ctypes.c_double * (rows * n))()
    stats = (ctypes.c_double * (rows * SAMPLER_COLS))()
    width = lib.stanli_wa_n_columns(handle)
    if width <= 0:
        width = lib.stanli_n_constrained(handle)
    values = None
    if 0 < rows * width <= MAX_VALUE_DOUBLES:
        values = (ctypes.c_double * (rows * width))()
    reports = (_SampleReport * 1)()
    err = ctypes.create_string_buffer(ERR_LEN)
    interrupted = ctypes.c_int(0)
    failed = lib.stanli_sample_multi_metric(
        handle, ctypes.byref(opts), ctypes.byref(metric), 0, chain, stats, values,
        _PROGRESS(), None, _POLL(), None, ctypes.byref(interrupted), reports, err, ERR_LEN)
    if failed:
        raise RuntimeError("sampling failed: " + err.value.decode(errors="replace"))
    return n, rows, chain, stats, reports[0]


def run_chain(lib_path, mir, data, seed, warmup, draws):
    lib = _bind(lib_path)
    handle = _model(lib, mir, data, seed)
    try:
        n, rows, chain, stats, report = _sample(lib, handle, seed, warmup, draws)
        columns = [stats[i * SAMPLER_COLS:(i + 1) * SAMPLER_COLS] for i in range(rows)]
        return dict(n_params=n, draws=list(chain),
                    seconds=dict(warmup=report.warmup_seconds, sampling=report.sampling_seconds),
                    n_leapfrog=[int(row[STAT_LEAPFROG]) for row in columns],
                    divergent=[int(row[STAT_DIVERGENT]) for row in columns],
                    treedepth=[int(row[STAT_TREEDEPTH]) for row in columns])
    finally:
        lib.stanli_model_free(handle)


def _phase_sums(values, warmup):
    return dict(warmup=int(sum(values[:warmup])), sampling=int(sum(values[warmup:])))


def _statistics(run, warmup):
    gradients = _phase_sums(run["n_leapfrog"], warmup)
    gradients["total"] = gradients["warmup"] + gradients["sampling"]
    return dict(gradients=gradients, divergences=_phase_sums(run["divergent"], warmup),
                max_treedepth=_phase_sums([int(d >= MAX_DEPTH) for d in run["treedepth"]], warmup))


def reference_run(lib_path, mir_path, data_path, seed, warmup, draws, points=POINTS):
    lib = _bind(lib_path)
    handle = _model(lib, mir_path, data_path, seed)
    n, rows, chain, stats, _ = _sample(lib, handle, seed, warmup, draws)
    run = dict(n_leapfrog=[int(stats[i * SAMPLER_COLS + STAT_LEAPFROG]) for i in range(rows)],
               divergent=[int(stats[i * SAMPLER_COLS + STAT_DIVERGENT]) for i in range(rows)],
               treedepth=[int(stats[i * SAMPLER_COLS + STAT_TREEDEPTH]) for i in range(rows)])
    result = dict(n_params=n, **_statistics(run, warmup))
    try:
        chosen = bp.select_iterations(run["n_leapfrog"], points)
    except ValueError as error:
        return dict(result, error=f"reference run did no work: {error}")
    selected = []
    for index in chosen:
        q = (ctypes.c_double * n)(*chain[index * n:(index + 1) * n])
        lp, grad = ctypes.c_double(), (ctypes.c_double * n)()
        status = lib.stanli_grad(handle, q, ctypes.byref(lp), grad)
        if status or not math.isfinite(lp.value) or not all(math.isfinite(g) for g in grad) \
                or not all(math.isfinite(v) for v in q):
            return dict(result, error=f"reference run point at iteration {index} has a non-finite "
                                      f"density or gradient on the real data")
        selected.append([float(v) for v in q])
    lib.stanli_model_free(handle)
    return dict(result, iterations=chosen, points=selected)


def _model_job(name, stan, data, lib, probe, seed, warmup, draws, timeout, points):
    base = dict(stan_sha256=bp.sha256(stan), data_sha256=bp.sha256(data))
    with tempfile.TemporaryDirectory(prefix="stanli-points-") as scratch:
        mir = pathlib.Path(scratch) / "model.sexp"
        try:
            compiled = subprocess.run(
                [str(probe), "--vectorize-loops", "on", "--output", str(mir), str(stan)],
                capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return name, dict(base, error=f"stanc timed out after {timeout} s")
        if compiled.returncode:
            return name, dict(base, error="stanc failed: " + compiled.stderr.strip()[:300])
        try:
            done = subprocess.run(
                [sys.executable, __file__, "--worker", str(lib), str(mir), str(data),
                 str(seed), str(warmup), str(draws), str(points)],
                capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return name, dict(base, error=f"reference run timed out after {timeout} s")
        if done.returncode:
            return name, dict(base, error=f"reference run failed with status {done.returncode}: "
                                          + done.stderr.strip()[-300:])
        return name, dict(base, **json.loads(done.stdout.splitlines()[-1]))


def _entry(result):
    keep = ("stan_sha256", "data_sha256", "gradients", "divergences", "max_treedepth")
    if "error" in result:
        return dict({k: result[k] for k in keep if k in result}, source="fixed", reason=result["error"])
    return dict({k: result[k] for k in keep + ("n_params", "iterations", "points")},
                source="reference_run")


def generate(cases, lib, probe, output, seed=1, warmup=1000, draws=1000, jobs=8, timeout=1800,
             points=POINTS):
    """cases maps model name to (stan path, data path)."""
    handle = _bind(lib)
    models = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [(name, pool.submit(_model_job, name, stan, data, lib, probe, seed, warmup, draws,
                                      timeout, points))
                   for name, (stan, data) in sorted(cases.items())]
        for name, future in futures:
            _, result = future.result()
            models[name] = _entry(result)
            note = models[name]["source"] if "error" not in result else "fixed: " + result["error"]
            print(f"{name}: {note}", file=sys.stderr, flush=True)
    doc = dict(schema=bp.SCHEMA,
               settings=dict(seed=seed, chains=1, warmup=warmup, draws=draws, delta=DELTA,
                             max_depth=MAX_DEPTH, save_warmup=True, points=points, timeout_s=timeout,
                             stanli_build_id=handle.stanli_build_id().decode(),
                             stanli_abi_version=handle.stanli_abi_version()),
               models=models)
    bp.write(output, doc)
    return doc


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        lib, mir, data, seed, warmup, draws, points = sys.argv[2:9]
        print(json.dumps(reference_run(lib, mir, data, int(seed), int(warmup), int(draws), int(points))))
        return 0
    from corpus_inventory import corpus_cases, materialize_data
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--lib", type=pathlib.Path, required=True, help="libstanli shared library")
    parser.add_argument("--probe", type=pathlib.Path, default=PROBE)
    parser.add_argument("--pdb", type=pathlib.Path, default=REPO / "deps/posteriordb")
    parser.add_argument("--output", type=pathlib.Path, default=bp.DEFAULT_PATH)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=1000)
    parser.add_argument("--draws", type=int, default=1000)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=1800, help="seconds per model")
    parser.add_argument("--filter", default="", help="only models whose name contains this text")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="stanli-points-data-") as scratch:
        cases = {}
        for name, case in corpus_cases(args.pdb, "all").items():
            if args.filter in name:
                cases[name] = (case.source, materialize_data(case, scratch))
        doc = generate(cases, args.lib, args.probe, args.output, args.seed, args.warmup,
                       args.draws, args.jobs, args.timeout)
    counts = {}
    for entry in doc["models"].values():
        counts[entry["source"]] = counts.get(entry["source"], 0) + 1
    print(", ".join(f"{n} {s}" for s, n in sorted(counts.items())) + f" -> {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
