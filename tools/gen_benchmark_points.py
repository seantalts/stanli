#!/usr/bin/env python3
"""Generate docs/internal/artifacts/benchmark-points.json.gz.

For every model that declares `int prior_only;` in its data block, sample the
prior with Stanli's NUTS (prior_only=1), keep ten evenly spaced draws as
unconstrained vectors, and require a finite density and gradient at each on the
real data (prior_only=0). A model that fails either step is recorded with the
reason and keeps the fixed points. Each model runs in its own process.

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
ERR_LEN = 4096
DEFAULT_MAX_ABS = 1000.0


def selected_draws(draws, points=POINTS):
    return [int((k + 0.5) * draws / points) for k in range(points)]


class _Opts(ctypes.Structure):
    _fields_ = [("seed", ctypes.c_uint32), ("threads_per_chain", ctypes.c_int),
                ("fast_math", ctypes.c_int)]


def _bind(path):
    lib = ctypes.CDLL(str(path))
    lib.stanli_model_new_with_opts.restype = ctypes.c_void_p
    lib.stanli_model_new_with_opts.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(_Opts),
                                               ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_model_free.argtypes = [ctypes.c_void_p]
    lib.stanli_n_unconstrained.restype = ctypes.c_int64
    lib.stanli_n_unconstrained.argtypes = [ctypes.c_void_p]
    lib.stanli_grad.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_double),
                                ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double)]
    lib.stanli_sample_stream_stats_init.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_double,
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_build_id.restype = ctypes.c_char_p
    lib.stanli_abi_version.restype = ctypes.c_int
    return lib


def _model(lib, mir, data, seed):
    opts = _Opts()
    opts.seed, opts.threads_per_chain, opts.fast_math = seed, 1, 0
    err = ctypes.create_string_buffer(ERR_LEN)
    handle = lib.stanli_model_new_with_opts(mir.encode(), json.dumps(data).encode(), ctypes.byref(opts),
                                            err, ERR_LEN)
    if not handle:
        raise RuntimeError("model construction failed: " + err.value.decode(errors="replace"))
    return handle


def sample_and_check(lib_path, mir_path, data_path, seed, warmup, draws, max_abs=None):
    lib = _bind(lib_path)
    mir = pathlib.Path(mir_path).read_text()
    data = json.loads(pathlib.Path(data_path).read_text())
    prior = _model(lib, mir, dict(data, prior_only=1), seed)
    real = _model(lib, mir, dict(data, prior_only=0), seed)
    n = lib.stanli_n_unconstrained(prior)
    if n < 1 or n != lib.stanli_n_unconstrained(real):
        return dict(error=f"parameter count differs between prior_only=1 ({n}) and prior_only=0")
    chain = (ctypes.c_double * (draws * n))()
    stats = (ctypes.c_double * (draws * SAMPLER_COLS))()
    err = ctypes.create_string_buffer(ERR_LEN)
    if lib.stanli_sample_stream_stats_init(prior, seed, warmup, draws, 0.8, None, chain, stats,
                                           None, None, err, ERR_LEN):
        return dict(error="prior sampling failed: " + err.value.decode(errors="replace"))
    if not all(math.isfinite(v) for v in chain):
        return dict(error="prior sampling produced a non-finite draw")
    chosen = selected_draws(draws)
    largest = max(abs(chain[i * n + j]) for i in chosen for j in range(n))
    if max_abs is not None and largest > max_abs:
        return dict(error=f"prior draws are unbounded: largest |q| {largest:.3g} exceeds {max_abs:g}")
    points = []
    for index in chosen:
        q = (ctypes.c_double * n)(*chain[index * n:(index + 1) * n])
        lp, grad = ctypes.c_double(), (ctypes.c_double * n)()
        status = lib.stanli_grad(real, q, ctypes.byref(lp), grad)
        if status or not math.isfinite(lp.value) or not all(math.isfinite(g) for g in grad):
            return dict(error=f"draw {index} has a non-finite density or gradient on the real data "
                              f"(largest |q| {max(abs(v) for v in q):.3g})")
        points.append([float(v) for v in q])
    lib.stanli_model_free(prior)
    lib.stanli_model_free(real)
    return dict(n_params=n, points=points)


def _model_job(name, stan, data, lib, probe, seed, warmup, draws, timeout, max_abs):
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
                 str(seed), str(warmup), str(draws), repr(max_abs)],
                capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return name, dict(base, error=f"sampling timed out after {timeout} s")
        if done.returncode:
            return name, dict(base, error=f"worker exited with status {done.returncode}: "
                                          + done.stderr.strip()[-300:])
        return name, dict(base, **json.loads(done.stdout.splitlines()[-1]))


def generate(cases, lib, probe, output, seed=1, warmup=1000, draws=1000, jobs=4, timeout=1800,
             max_abs=DEFAULT_MAX_ABS):
    """cases maps model name to (stan path, data path); non-prior models are skipped."""
    chosen = {name: paths for name, paths in sorted(cases.items())
              if bp.prior_only_model(pathlib.Path(paths[0]).read_text())}
    lib_handle = _bind(lib)
    models, fallbacks = {}, {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(_model_job, name, stan, data, lib, probe, seed, warmup, draws, timeout,
                              max_abs)
                   for name, (stan, data) in chosen.items()]
        for future in futures:
            name, result = future.result()
            hashes = dict(stan_sha256=result["stan_sha256"], data_sha256=result["data_sha256"])
            if "error" in result:
                fallbacks[name] = dict(hashes, reason=result["error"])
            else:
                models[name] = dict(hashes, source="prior", n_params=result["n_params"],
                                    points=result["points"])
            print(f"{name}: {'prior' if name in models else 'fallback: ' + fallbacks[name]['reason']}",
                  file=sys.stderr, flush=True)
    doc = dict(schema=bp.SCHEMA,
               settings=dict(seed=seed, warmup=warmup, draws=draws, chains=1, delta=0.8,
                             selected_draws=selected_draws(draws), max_abs_q=max_abs,
                             stanli_build_id=lib_handle.stanli_build_id().decode(),
                             stanli_abi_version=lib_handle.stanli_abi_version()),
               models=models, fallbacks=fallbacks)
    bp.write(output, doc)
    return doc


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        lib, mir, data, seed, warmup, draws, max_abs = sys.argv[2:9]
        print(json.dumps(sample_and_check(lib, mir, data, int(seed), int(warmup), int(draws),
                                          None if max_abs == "None" else float(max_abs))))
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
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--max-abs", type=float, default=DEFAULT_MAX_ABS,
                        help="reject a model whose selected draws exceed this on the unconstrained "
                             "scale (an improper prior); 0 disables")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--filter", default="", help="only models whose name contains this text")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="stanli-points-data-") as scratch:
        cases = {}
        for name, case in corpus_cases(args.pdb, "all").items():
            if args.filter in name:
                cases[name] = (case.source, materialize_data(case, scratch))
        doc = generate(cases, args.lib, args.probe, args.output, args.seed, args.warmup,
                       args.draws, args.jobs, args.timeout, args.max_abs or None)
    print(f"{len(doc['models'])} prior, {len(doc['fallbacks'])} fallback -> {args.output}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
