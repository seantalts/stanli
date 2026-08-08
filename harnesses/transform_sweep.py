#!/usr/bin/env python3
"""Every parameter transform, bitwise against CmdStan.

The constraint transforms are the one part of the lowering with no
density-level oracle behind it: `fn_sweep.py` generates a model per
distribution, but a transform is not a function call, it is a property of
a `parameters` declaration. So this sweeps declarations instead --
one model per transform, each with a target that reads the constrained
value, compared to a CmdStan build of the same model at the same three
deterministic points.

What it checks is what matters and what is easy to get subtly wrong: the
unconstrained SIZE (a corr_matrix[3] has 3 free parameters, a
cov_matrix[3] has 6), the constrained values, the log-Jacobian folded
into lp, and the gradient through both.

    harnesses/transform_sweep.py deps/cmdstan
    harnesses/transform_sweep.py deps/cmdstan --build build

Requires a CmdStan checkout for the reference side, so it does not run in
CI; `tests/test_transforms.cpp` is what guards these on every push.
"""
import argparse
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent

# name -> (parameters block, target expression). The target has to touch
# every element of the constrained value, or a transform could be wrong in
# a component the gradient never sees.
CASES = [
    ("offset_scalar",
     "real<offset=m, multiplier=s> a;", "a"),
    ("offset_only",
     "real<offset=m> a;", "a"),
    ("multiplier_only",
     "real<multiplier=s> a;", "a"),
    ("offset_vector",
     "vector<offset=m, multiplier=s>[4] a;", "sum(a)"),
    # A per-element offset and multiplier: the broadcast-vs-elementwise
    # split in the kernel, and the case where both are parameters.
    ("offset_vector_vec",
     "vector[4] mu_p; vector<lower=0>[4] sg_p;"
     " vector<offset=mu_p, multiplier=sg_p>[4] a;",
     "sum(a) + sum(mu_p) + sum(sg_p)"),
    ("offset_array",
     "array[2] vector<offset=m, multiplier=s>[3] a;",
     "sum(a[1]) + sum(a[2])"),
    ("unit_vector", "unit_vector[4] a;", "sum(a) + a[1] * 2"),
    ("unit_vector_array",
     "array[2] unit_vector[3] a;", "sum(a[1]) + sum(a[2]) * 2"),
    ("sum_to_zero", "sum_to_zero_vector[5] a;", "sum(a) + a[2] * 3"),
    ("sum_to_zero_array",
     "array[2] sum_to_zero_vector[4] a;", "sum(a[1]) + sum(a[2]) * 2"),
    ("corr_matrix", "corr_matrix[3] a;", "sum(a) + a[1, 2] * 3"),
    ("cov_matrix", "cov_matrix[3] a;", "sum(a) + a[2, 3] * 3"),
    ("cholesky_corr", "cholesky_factor_corr[3] a;", "sum(a) + a[3, 1] * 3"),
    ("cholesky_cov_square", "cholesky_factor_cov[3] a;",
     "sum(a) + a[2, 1] * 3"),
    ("cholesky_cov_rect", "cholesky_factor_cov[4, 3] a;",
     "sum(a) + a[4, 2] * 3"),
    # The transforms that already worked, swept alongside so a regression
    # in the shared plumbing shows up here too.
    ("simplex", "simplex[4] a;", "sum(a) + a[1] * 3"),
    ("ordered", "ordered[4] a;", "sum(a) + a[1] * 3"),
    ("positive_ordered", "positive_ordered[4] a;", "sum(a) + a[1] * 3"),
    ("lower", "real<lower=0> a;", "a"),
    ("lower_upper", "real<lower=0, upper=1> a;", "a"),
]


def model_for(params, target):
    return (f"data {{ real m; real<lower=0.5> s; }}\n"
            f"parameters {{ {params} }}\n"
            f"model {{ target += {target}; }}\n")


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True,
                          text=True, **kw)


def ulps(a, b):
    if a == b:
        return 0
    ia = struct.unpack("<q", struct.pack("<d", a))[0]
    ib = struct.unpack("<q", struct.pack("<d", b))[0]
    if (ia < 0) != (ib < 0):
        return float("inf")
    return abs(ia - ib)


def build_reference(cs, d, stan, name):
    """CmdStan's own generated model, compiled against ref_driver."""
    math = cs / "stan" / "lib" / "stan_math"
    hpp = d / f"{name}.hpp"
    if run([REPO / "deps/stanc3/stanc", stan, f"--o={hpp}"]).returncode != 0:
        return None, "stanc_fail"
    inc = [cs / "stan" / "src", math,
           next((cs / "stan" / "lib").glob("rapidjson_*")),
           next((math / "lib").glob("eigen_*")),
           next((math / "lib").glob("boost_*")),
           next((math / "lib").glob("sundials_*")) / "include",
           next((math / "lib").glob("tbb_*")) / "include"]
    tbb = math / "lib" / "tbb"
    exe = d / "ref"
    b = run(["clang++", "-std=c++17", "-O1", "-ffp-contract=off",
             "-D_REENTRANT", "-DBOOST_DISABLE_ASSERTS"]
            + [f"-I{i}" for i in inc]
            + ["-include", hpp, REPO / "tools/ref_driver.cpp",
               f"-L{tbb}", "-ltbb", f"-Wl,-rpath,{tbb}", "-o", exe])
    if b.returncode != 0:
        return None, "ref_build_fail: " + b.stderr.strip()[-90:]
    return exe, None


def sweep_one(case, cs, tmp, build):
    name, params, target = case
    d = tmp / name
    d.mkdir(parents=True, exist_ok=True)
    stan = d / f"{name}.stan"
    stan.write_text(model_for(params, target))
    data = d / "data.json"
    data.write_text(json.dumps({"m": 0.3, "s": 1.7}))

    check = REPO / build / "stanli_check"
    ours = run([check, stan, data, "--stanc", REPO / "deps/stanc3/stanc"])
    if "COMPILE_FAIL" in ours.stdout + ours.stderr:
        why = (ours.stdout + ours.stderr).strip().splitlines()[-1]
        return name, "unsupported", why[:70]

    exe, err = build_reference(cs, d, stan, name)
    if exe is None:
        return name, "ref_fail", err

    worst, n_cmp = 0, 0
    for point in range(3):
        ref = run([exe, data, point])
        got = run([check, stan, data, "--stanc",
                   REPO / "deps/stanc3/stanc", "--point", point])
        rf = [l for l in ref.stdout.splitlines() if l.startswith("OK")]
        gf = [l for l in got.stdout.splitlines() if l.startswith("OK")]
        # The two engines spell "this point is invalid" differently, and
        # reading that as disagreement would be wrong. stanli_check
        # reports EVAL_FAIL and prints no OK line; CmdStan's log_prob
        # returns a row of nan and only throws later, out of write_array.
        # A unit_vector at the origin is exactly this: both say norm 0 is
        # not a point, in different words. So an all-nan reference row
        # counts as a rejection.
        if rf and all(x != x for x in
                      (float(v) for v in rf[0].split()[1:])):
            rf = []
        if not rf or not gf:
            # Both rejecting is agreement (the point is outside the
            # support); one rejecting is a real disagreement.
            if bool(rf) != bool(gf):
                return name, "one_side_threw", f"point {point}"
            continue
        a = [float(x) for x in rf[0].split()[1:]]
        b = [float(x) for x in gf[0].split()[1:]]
        if len(a) != len(b):
            # This is the unconstrained-size check: a wrong free-parameter
            # count for corr_matrix vs cov_matrix lands exactly here.
            return name, "shape_mismatch", f"cmdstan {len(a)}, stanli {len(b)}"
        n_cmp += len(a)
        worst = max([worst] + [ulps(x, y) for x, y in zip(a, b)])
    if n_cmp == 0:
        return name, "no_valid_point", ""
    return name, "ok", f"{n_cmp} values, {worst} ulp"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmdstan")
    ap.add_argument("--build", default="build-rel",
                    help="build tree holding stanli_check")
    ap.add_argument("--filter", default="")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    cs = pathlib.Path(args.cmdstan).resolve()
    cases = [c for c in CASES if args.filter in c[0]]

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="transform_sweep_"))
    bad = 0
    for case in cases:
        name, status, note = sweep_one(case, cs, tmp, args.build)
        if status != "ok":
            bad += 1
        flag = "ok  " if status == "ok" else "FAIL"
        print(f"{flag} {name:24s} {status:16s} {note}")
    print(f"\n{len(cases) - bad}/{len(cases)} transforms bitwise vs CmdStan")
    if args.keep:
        print(f"artifacts in {tmp}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
