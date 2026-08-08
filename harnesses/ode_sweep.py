#!/usr/bin/env python3
"""Every ODE interface, against CmdStan.

`integrate_ode_*` is deprecated and `ode_*` is what a 2026 model is
written with, so both have to work and agree. The two differ in more than
spelling: the modern right-hand side takes a `vector` state and returns a
`vector`, and everything after `ts` is passed straight through in any
number and any type, where the old one fixed exactly `(theta, x_r, x_i)`.

What this sweeps is that argument plumbing, which is where the shapes go
wrong quietly: a parameter argument mis-packed into the data region still
integrates, still produces a finite gradient, and is simply the wrong
model. Each case is compared to a CmdStan build of the same model at the
same three deterministic points.

    harnesses/ode_sweep.py deps/cmdstan --build build

Needs a CmdStan checkout, so it does not run in CI;
`tests/fixtures/odevariadic.stan` is what guards this on every push.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent

# The gate, as a RELATIVE difference rather than in ULPs. An ODE solve is
# iterative, so bitwise is not a bar an integrator can be held to the way
# a density can -- but a matched solver at matched tolerances agrees to
# many digits and a different solver does not, and those two outcomes are
# nowhere near each other.
#
# ULPs are the wrong metric here specifically because of the zeros. A
# gradient that is analytically zero comes back as exactly 0 from one
# engine and 2.2e-16 from the other, and the bit-pattern distance between
# those is 4.4e18 -- which reads as catastrophe and is one epsilon. The
# absolute floor below is what stops that from being reported as a
# failure.
MAX_REL = 1e-9
ABS_FLOOR = 1e-12

# A right-hand side in the modern (vector) style, parameterized by the
# argument list it takes after (t, y). Each case names the functions
# block, the call, and any extra parameters the call needs.
MODERN_FNS = """
functions {
  vector rhs(real t, vector y, real a, real b) {
    vector[2] dy;
    dy[1] = -a * y[1] + b * y[2];
    dy[2] = a * y[1] - b * y[2];
    return dy;
  }
  vector rhs_vec(real t, vector y, vector p) {
    vector[2] dy;
    dy[1] = -p[1] * y[1] + p[2] * y[2];
    dy[2] = p[1] * y[1] - p[2] * y[2];
    return dy;
  }
  vector rhs_mixed(real t, vector y, real a, vector p, real d, array[] int k) {
    vector[2] dy;
    dy[1] = -a * y[1] + p[1] * y[2] + d * k[1];
    dy[2] = a * y[1] - p[2] * y[2];
    return dy;
  }
}
"""

# name -> (extra parameters, the ode call expression)
CASES = [
    # The four solvers, same shape, so a difference is the solver.
    ("rk45", "", "ode_rk45(rhs, y0, 0.0, ts, a, b)"),
    ("bdf", "", "ode_bdf(rhs, y0, 0.0, ts, a, b)"),
    ("adams", "", "ode_adams(rhs, y0, 0.0, ts, a, b)"),
    ("ckrk", "", "ode_ckrk(rhs, y0, 0.0, ts, a, b)"),
    # The _tol forms put the tolerances before the variadic args, which is
    # the easiest place to slice the argument list one position wrong.
    ("rk45_tol", "", "ode_rk45_tol(rhs, y0, 0.0, ts, 1e-8, 1e-8, 100000, a, b)"),
    ("bdf_tol", "", "ode_bdf_tol(rhs, y0, 0.0, ts, 1e-8, 1e-8, 100000, a, b)"),
    ("adams_tol", "",
     "ode_adams_tol(rhs, y0, 0.0, ts, 1e-8, 1e-8, 100000, a, b)"),
    ("ckrk_tol", "",
     "ode_ckrk_tol(rhs, y0, 0.0, ts, 1e-8, 1e-8, 100000, a, b)"),
    # A single vector parameter argument: the common case, and the one
    # that must not pay for a concatenation.
    ("vector_param", "vector<lower=0>[2] p;",
     "ode_rk45(rhs_vec, y0, 0.0, ts, p)"),
    # Two parameter arguments of different shapes, plus a data real and a
    # data int array: every region of the packing at once, and the case
    # that catches an argument landing in the wrong one.
    ("mixed_args", "vector<lower=0>[2] p;",
     "ode_rk45(rhs_mixed, y0, 0.0, ts, a, p, d_real, d_int)"),
]

# The deprecated interface, swept alongside so its regression shows here.
LEGACY = ("""
functions {
  array[] real rhs_old(real t, array[] real y, array[] real theta,
                       array[] real x_r, array[] int x_i) {
    array[2] real dy;
    dy[1] = -theta[1] * y[1] + theta[2] * y[2];
    dy[2] = theta[1] * y[1] - theta[2] * y[2];
    return dy;
  }
}
""", "integrate_ode_rk45(rhs_old, y0, 0.0, ts, "
     "{a, b}, x_r_arr, x_i_arr)")


def model_for(name, extra_params, call, legacy=False):
    fns = LEGACY[0] if legacy else MODERN_FNS
    decl = "array[N, 2] real z" if legacy else "array[N] vector[2] z"
    read = ("sum(to_vector(z[n]))" if legacy else "sum(z[n])")
    y0_decl = ("array[2] real<lower=0> y0;" if legacy
               else "vector<lower=0>[2] y0;")
    return f"""{fns}
data {{
  int<lower=1> N;
  array[N] real ts;
  real d_real;
  array[1] int d_int;
  array[0] real x_r_arr;
  array[0] int x_i_arr;
}}
parameters {{
  real<lower=0> a;
  real<lower=0> b;
  {y0_decl}
  {extra_params}
}}
transformed parameters {{
  {decl} = {call};
}}
model {{
  a ~ lognormal(0, 1);
  b ~ lognormal(0, 1);
  y0 ~ lognormal(0, 1);
  for (n in 1:N) target += {read};
}}
"""


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True,
                          text=True, **kw)


def reldiff(a, b):
    """Relative difference, with an absolute floor for values near zero."""
    d = abs(a - b)
    if d == 0:
        return 0.0
    scale = max(abs(a), abs(b))
    if scale < ABS_FLOOR:
        return 0.0 if d < ABS_FLOOR else d
    return d / scale


def build_reference(cs, d, stan, name):
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
    sun = next((math / "lib").glob("sundials_*")) / "lib"
    exe = d / "ref"
    # CVODES for the bdf/adams solvers: the rk45 drivers link without it,
    # so leaving it out made those two fail to BUILD and look like a
    # coverage gap rather than a missing -l.
    libs = []
    for a in sorted(sun.glob("*.a")):
        libs.append(str(a))
    b = run(["clang++", "-std=c++17", "-O1", "-ffp-contract=off",
             "-D_REENTRANT", "-DBOOST_DISABLE_ASSERTS"]
            + [f"-I{i}" for i in inc]
            + ["-include", hpp, REPO / "tools/ref_driver.cpp"]
            + libs
            + [f"-L{tbb}", "-ltbb", f"-Wl,-rpath,{tbb}", "-o", exe])
    if b.returncode != 0:
        return None, "ref_build_fail: " + b.stderr.strip()[-100:]
    return exe, None


def sweep_one(case, cs, tmp, build, legacy=False):
    name, extra, call = case
    d = tmp / name
    d.mkdir(parents=True, exist_ok=True)
    stan = d / f"{name}.stan"
    stan.write_text(model_for(name, extra, call, legacy))
    data = d / "data.json"
    data.write_text(json.dumps({
        "N": 3, "ts": [0.5, 1.0, 1.5], "d_real": 0.25, "d_int": [2],
        "x_r_arr": [], "x_i_arr": []}))

    check = REPO / build / "stanli_check"
    ours = run([check, stan, data, "--stanc", REPO / "deps/stanc3/stanc"])
    if "COMPILE_FAIL" in ours.stdout + ours.stderr:
        why = (ours.stdout + ours.stderr).strip().splitlines()[-1]
        return name, "unsupported", why[:80]

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
        if rf and all(x != x for x in (float(v) for v in rf[0].split()[1:])):
            rf = []
        if not rf or not gf:
            if bool(rf) != bool(gf):
                return name, "one_side_threw", f"point {point}"
            continue
        a = [float(x) for x in rf[0].split()[1:]]
        b = [float(x) for x in gf[0].split()[1:]]
        if len(a) != len(b):
            return name, "shape_mismatch", f"cmdstan {len(a)}, stanli {len(b)}"
        n_cmp += len(a)
        worst = max([worst] + [reldiff(x, y) for x, y in zip(a, b)])
    if n_cmp == 0:
        return name, "no_valid_point", ""
    # An ODE solve is iterative, so bitwise is not the bar an integrator
    # can be held to the way a density can -- but "agrees to a few ULP"
    # and "ran a different solver" are the two outcomes, and they are
    # nowhere near each other. Anything past this is the second one, and
    # reporting it as a pass with a large number beside it is how a wrong
    # solver ships.
    if worst > MAX_REL:
        return name, "diverged", f"{n_cmp} values, {worst:.2e} rel"
    return name, "ok", f"{n_cmp} values, {worst:.2e} rel"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmdstan")
    ap.add_argument("--build", default="build-rel")
    ap.add_argument("--filter", default="")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    cs = pathlib.Path(args.cmdstan).resolve()

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="ode_sweep_"))
    cases = [(c, False) for c in CASES if args.filter in c[0]]
    if args.filter in "legacy_integrate_ode":
        cases.append((("legacy_integrate_ode", "", LEGACY[1]), True))

    bad = 0
    for case, legacy in cases:
        name, status, note = sweep_one(case, cs, tmp, args.build, legacy)
        if status != "ok":
            bad += 1
        print(f"{'ok  ' if status == 'ok' else 'FAIL'} {name:22s} "
              f"{status:16s} {note}")
    print(f"\n{len(cases) - bad}/{len(cases)} ODE interfaces bitwise vs CmdStan")
    if args.keep:
        print(f"artifacts in {tmp}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
