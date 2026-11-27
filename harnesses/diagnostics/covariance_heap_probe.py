#!/usr/bin/env python3
"""Darwin phase-level heap probe; isolated workers use the public C ABI.

Example: python3 harnesses/diagnostics/covariance_heap_probe.py \
  --runtime /absolute/path/libstanli.dylib --variant udf --size 256 --output /tmp/probe.json
The driver retains exact source/data, MIR/library hashes, phase timings, live
malloc bytes, allocator reservations, RSS, and full-output byte hashes. RSS can
remain high after free; size_in_use measures allocator-live bytes, not ownership.
"""
import argparse
import ctypes as c
import gc
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import subprocess
import sys
import time


class Stats(c.Structure):
    _fields_ = [("blocks_in_use", c.c_uint), ("size_in_use", c.c_size_t),
                ("max_size_in_use", c.c_size_t), ("size_allocated", c.c_size_t)]


class Options(c.Structure):
    _fields_ = [("seed", c.c_uint32), ("threads", c.c_int),
                ("fast_math", c.c_int), ("allow_interpreter", c.c_int),
                ("execution_report", c.c_int), ("parallel_write_array", c.c_int),
                ("parallel_blas", c.c_int)]


def worker(args):
    resource.setrlimit(resource.RLIMIT_CPU, (30, 30))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    system = c.CDLL("/usr/lib/libSystem.B.dylib")
    system.malloc_zone_statistics.argtypes = [c.c_void_p, c.POINTER(Stats)]
    rows = []

    def record(stage, elapsed=0.0, **extra):
        gc.collect()
        rss = int(subprocess.check_output(["/bin/ps", "-o", "rss=", "-p", str(os.getpid())])) * 1024
        stats = Stats()
        system.malloc_zone_statistics(None, c.byref(stats))
        row = dict(stage=stage, elapsed_seconds=elapsed, current_rss_bytes=rss,
                   peak_rss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                   **{key: getattr(stats, key) for key, _ in Stats._fields_}, **extra)
        rows.append(row)

    record("python")
    start = time.perf_counter()
    lib = c.CDLL(args.runtime)
    lib.stanli_build_id.restype = c.c_char_p
    lib.stanli_stan_to_mir_with_opts.argtypes = [c.c_char_p, c.c_void_p, c.c_size_t, c.c_int, c.c_char_p, c.c_size_t]
    lib.stanli_stan_to_mir_with_opts.restype = c.c_void_p
    lib.stanli_string_free.argtypes = [c.c_void_p]
    lib.stanli_model_new_with_opts.argtypes = [c.c_char_p, c.c_char_p, c.c_void_p, c.c_char_p, c.c_size_t]
    lib.stanli_model_new_with_opts.restype = c.c_void_p
    lib.stanli_model_free.argtypes = [c.c_void_p]
    for name in ("stanli_n_unconstrained", "stanli_wa_n_columns"):
        getattr(lib, name).argtypes = [c.c_void_p]
        getattr(lib, name).restype = c.c_int64
    lib.stanli_wa_row.argtypes = [c.c_void_p, c.POINTER(c.c_double), c.POINTER(c.c_double)]
    lib.stanli_grad_err.argtypes = [c.c_void_p, c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_char_p, c.c_size_t]
    record("library", time.perf_counter() - start)
    if args.variant == "normal_canary":
        source = b"data { int<lower=0> N; vector[N] y; } parameters { real mu; } model { mu ~ normal(0,1); y ~ normal(mu,1); } generated quantities { real square_mu = square(mu); }"
        data = dict(N=args.size, y=[math.sin(i) for i in range(args.size)])
    else:
        source = (Path(__file__).parent / "fixtures" / ("covariance_" + args.variant + ".stan")).read_bytes()
        data = dict(N=args.size, x=[i / max(1, args.size - 1) for i in range(args.size)],
                    y=[math.sin(i) for i in range(args.size)], jitter=0.1,
                    selector=1, permutation=list(range(args.size, 0, -1)))
    encoded = json.dumps(data).encode()
    err = c.create_string_buffer(8192)
    start = time.perf_counter()
    compiled = lib.stanli_stan_to_mir_with_opts(source, None, 0, 0, err, len(err))
    if not compiled:
        raise RuntimeError(err.value.decode())
    mir = c.string_at(compiled)
    lib.stanli_string_free(compiled)
    record("compile", time.perf_counter() - start)
    start = time.perf_counter()
    options = Options(1, 1, 0, 1, 1, 0) if args.execution_report else None
    model = lib.stanli_model_new_with_opts(mir, encoded, c.byref(options) if options else None, err, len(err))
    if not model:
        raise RuntimeError(err.value.decode())
    record("prepare", time.perf_counter() - start)
    n = lib.stanli_n_unconstrained(model)
    cols = lib.stanli_wa_n_columns(model)
    q = (c.c_double * n)(*[0.1 + 0.05 * (i % 7) - 0.15 * (i % 3) for i in range(n)])
    grad = (c.c_double * n)()
    lp = c.c_double()
    for repeat in range(3):
        start = time.perf_counter()
        status = lib.stanli_grad_err(model, q, c.byref(lp), grad, err, len(err))
        if status:
            raise RuntimeError(err.value.decode())
        record("gradient" + str(repeat + 1), time.perf_counter() - start,
               lp=lp.value, gradient=list(grad),
               lp_gradient_sha256=hashlib.sha256(bytes(lp) + bytes(grad)).hexdigest())
    output = (c.c_double * cols)()
    for repeat in range(3):
        if repeat == 2:
            q[0] += 0.125
        start = time.perf_counter()
        status = lib.stanli_wa_row(model, q, output)
        elapsed = time.perf_counter() - start
        if status:
            raise RuntimeError("write_array status " + str(status))
        record("write_array" + str(repeat + 1), elapsed,
               q_sha256=hashlib.sha256(bytes(q)).hexdigest(),
               output_sha256=hashlib.sha256(bytes(output)).hexdigest(),
               all_finite=all(math.isfinite(v) for v in output))
    del output, q, grad
    start = time.perf_counter()
    lib.stanli_model_free(model)
    record("free_model", time.perf_counter() - start)
    print(json.dumps(dict(variant=args.variant, N=args.size,
                         runtime_path=str(Path(args.runtime).resolve()),
                         runtime_sha256=hashlib.sha256(Path(args.runtime).read_bytes()).hexdigest(),
                         harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                         execution_report=args.execution_report,
                         build_id=lib.stanli_build_id().decode(), source=source.decode(),
                         data=data, source_sha256=hashlib.sha256(source).hexdigest(),
                         mir_sha256=hashlib.sha256(mir).hexdigest(),
                         n_unconstrained=n, output_columns=cols, observations=rows)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--variant", choices=["udf", "builtin_equivalent", "normal_canary"], required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--worker", action="store_true")
    parser.add_argument("--execution-report", action="store_true",
                        help="retain a public ABI execution manifest in .stderr; do not compare these instrumented timings")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This probe uses Darwin malloc_zone_statistics and RSS units")
    if args.worker:
        worker(args)
        return
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    # A manifest can exceed pipe capacity. Keep both streams in files so a
    # correct worker cannot deadlock before the supervising guard reads them.
    stdout_path = destination.with_suffix(destination.suffix + ".stdout")
    stderr_path = destination.with_suffix(destination.suffix + ".stderr")
    stdout_file = stdout_path.open("w")
    stderr_file = stderr_path.open("w")
    proc = subprocess.Popen([sys.executable, __file__, *sys.argv[1:], "--worker"],
                            stdout=stdout_file, stderr=stderr_file, text=True)
    start = time.monotonic()
    reason = None
    while proc.poll() is None:
        if time.monotonic() - start > 45:
            reason = "45-second wall timeout"
        try:
            rss = int(subprocess.check_output(["/bin/ps", "-o", "rss=", "-p", str(proc.pid)])) * 1024
            if rss > 1024 ** 3:
                reason = "1-GiB RSS guard"
        except (subprocess.CalledProcessError, ValueError):
            pass
        if reason:
            proc.kill()
            break
        time.sleep(0.05)
    proc.wait()
    stdout_file.close()
    stderr_file.close()
    stdout = stdout_path.read_text()
    stderr = stderr_path.read_text()
    if proc.returncode or reason:
        destination.write_text(json.dumps(dict(error=reason, returncode=proc.returncode, stdout=stdout, stderr=stderr)))
        raise SystemExit(reason or "worker failed: " + stderr)
    json.loads(stdout)
    destination.write_text(stdout)


if __name__ == "__main__":
    main()
