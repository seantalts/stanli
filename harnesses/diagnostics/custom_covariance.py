#!/usr/bin/env python3
"""Bounded #656 diagnostic surrogate; this is not the reporter's model.

Uses the runtime C ABI without NumPy, compiles one independent CmdStan
reference, and compares densities/gradients at the corpus's three standard
points. Also checks rejection for non-positive-definite/asymmetric matrices.
No sampling or performance claim is made.
"""
import argparse
import ctypes as c
import hashlib
import json
import math
import pathlib
import platform
import struct
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import compile_cmd


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def revision(path):
    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"],
                                   text=True).strip()


def qpoint(n, point):
    return [(0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)) if point == 0
            else .02 * ((i % 5) - 2) if point == 1 else 0.0 for i in range(n)]


def ulps(a, b):
    def ordered(x):
        v = struct.unpack(">Q", struct.pack(">d", x))[0]
        return ~v & ((1 << 64) - 1) if v >> 63 else v | (1 << 63)
    return 0 if a == b else abs(ordered(a) - ordered(b))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runtime", required=True, type=pathlib.Path)
    ap.add_argument("--cmdstan", required=True, type=pathlib.Path)
    ap.add_argument("--stanc", required=True, type=pathlib.Path)
    ap.add_argument("--work", required=True, type=pathlib.Path)
    ap.add_argument("--output", required=True, type=pathlib.Path)
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    source = pathlib.Path(__file__).with_suffix(".stan")
    header = a.work / "custom_covariance.hpp"
    exe = a.work / "custom_covariance_ref"
    commands = [[str(a.stanc), str(source), f"--o={header}"],
                compile_cmd(a.cmdstan, header, REPO / "tools/ref_driver.cpp",
                            exe, opt="-O1", sundials=False)]
    for i, cmd in enumerate(commands):
        result = subprocess.run(cmd, capture_output=True, text=True)
        (a.work / f"build-{i}.log").write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end="")
        result.check_returncode()

    lib = c.CDLL(str(a.runtime.resolve()))
    lib.stanli_build_id.restype = c.c_char_p
    lib.stanli_stan_to_mir_with_opts.argtypes = [c.c_char_p, c.c_void_p,
        c.c_size_t, c.c_int, c.c_char_p, c.c_size_t]
    lib.stanli_stan_to_mir_with_opts.restype = c.c_void_p
    lib.stanli_string_free.argtypes = [c.c_void_p]
    lib.stanli_model_new_with_opts.argtypes = [c.c_char_p, c.c_char_p,
        c.c_void_p, c.c_char_p, c.c_size_t]
    lib.stanli_model_new_with_opts.restype = c.c_void_p
    lib.stanli_model_free.argtypes = [c.c_void_p]
    lib.stanli_n_unconstrained.argtypes = [c.c_void_p]
    lib.stanli_n_unconstrained.restype = c.c_int64
    lib.stanli_grad_err.argtypes = [c.c_void_p, c.POINTER(c.c_double),
        c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_char_p, c.c_size_t]
    err = c.create_string_buffer(8192)
    t = time.perf_counter()
    compiled = lib.stanli_stan_to_mir_with_opts(source.read_bytes(), None, 0,
                                              0, err, len(err))
    if not compiled:
        raise RuntimeError(err.value.decode())
    mir = c.string_at(compiled)
    lib.stanli_string_free(compiled)
    report = dict(source_sha=revision(REPO), runtime=lib.stanli_build_id().decode(),
                  runtime_sha256=sha(a.runtime), model_sha256=sha(source),
                  stanc_sha256=sha(a.stanc), cmdstan_sha=revision(a.cmdstan),
                  stan_sha=revision(a.cmdstan / "stan"),
                  math_sha=revision(a.cmdstan / "stan/lib/stan_math"),
                  platform=platform.platform(), commands=commands,
                  compilation_seconds=time.perf_counter() - t, cases=[])
    for n, invalid in [(1, 0), (3, 0), (40, 0), (3, 1), (3, 2)]:
        data = dict(N=n, x=[i / max(1, n - 1) for i in range(n)],
                    y=[math.sin(i) for i in range(n)], invalid=invalid)
        data_path = a.work / f"n{n}-invalid{invalid}.json"
        data_path.write_text(json.dumps(data))
        err.value = b""
        t = time.perf_counter()
        model = lib.stanli_model_new_with_opts(mir, data_path.read_bytes(), None,
                                              err, len(err))
        case = dict(N=n, invalid=invalid, preparation_seconds=time.perf_counter()-t,
                    preparation_error=err.value.decode(), points=[])
        if model:
            dim = lib.stanli_n_unconstrained(model)
            for p in range(3):
                q = (c.c_double * dim)(*qpoint(dim, p))
                grad, lp = (c.c_double * dim)(), c.c_double()
                err.value = b""
                status = lib.stanli_grad_err(model, q, c.byref(lp), grad,
                                             err, len(err))
                ref = subprocess.run([str(exe), str(data_path), str(p)],
                                     capture_output=True, text=True)
                row = dict(point=p, status=status, error=err.value.decode(),
                           reference_stdout=ref.stdout, reference_stderr=ref.stderr,
                           reference_exit=ref.returncode)
                ok = next((s for s in ref.stdout.splitlines() if s.startswith("OK ")), None)
                if not status:
                    values = [lp.value] + list(grad)
                    row["values"] = values
                    if ok:
                        expected = [float(v) for v in ok.split()[1:]]
                        assert len(values) == len(expected)
                        row["max_ulp"] = max(ulps(x,y) for x,y in zip(values,expected))
                        row["max_scaled_error"] = max(abs(x-y)/(1+abs(y)) for x,y in zip(values,expected))
                row["matching_classification"] = ((status == 0 and ok is not None)
                    or (status == 1 and "EVAL_FAIL" in ref.stdout))
                case["points"].append(row)
            lib.stanli_model_free(model)
        report["cases"].append(case)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(report, indent=2) + "\n")
    for case in report["cases"]:
        print(json.dumps({k:v for k,v in case.items() if k != "points"}))
        for p in case["points"]:
            print(json.dumps({k:v for k,v in p.items() if k not in ["values", "reference_stdout", "reference_stderr"]}))


if __name__ == "__main__":
    main()
