#!/usr/bin/env python3
"""Per-function coverage and parity against CmdStan.

The corpus tells us the 120 models we care about are right. It says
nothing about a function no corpus model happens to use, and "supported"
in this project has twice meant "listed in a table no path could reach"
-- exponential_lpdf inside a parameter-dependent region was accepted by
the name lookup and refused by the block above it, silently, for a week.

This is the oracle for that. One tiny model per function, data chosen to
sit in the distribution's support, lowered by stanli and compiled by
CmdStan, both evaluated at the same points through the drivers the corpus
already uses (build-rel/stanli_check and tools/ref_driver.cpp). Reports
one line per function: supported or not, worst ULP against CmdStan, and
our per-gradient time so a new arrival that is accidentally quadratic is
visible immediately.

stanc3's own test corpus would be the obvious source of models, and is
not usable here: those files carry no data, and this lowering evaluates
transformed data eagerly and unrolls loops against known bounds, so a
model without data cannot be lowered at all. Generating the models is
what makes the data problem go away.

Usage:
  harnesses/fn_sweep.py deps/cmdstan [--filter SUBSTR] [--jobs N]
                                     [--keep] [--missing]

  --missing   also emit models for functions stan-math has and stanli
              does not, so the report doubles as the coverage gap.
  --from-stanc  take the function list from stanc3 itself
                (--dump-stan-math-signatures, 564 names / 24k signatures)
                rather than the table below: every function with an
                all-real-scalar signature returning real. That is the
                authoritative list, so the report is a true coverage
                number rather than a number about what someone typed here.
"""
import argparse
import concurrent.futures
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import build_reference, compare_points  # noqa: E402

# Which build tree to test. --build points it elsewhere; the default
# is the Release tree the wheel ships from.
BUILD = "build-rel"

# (stan name, arg count, argument values). The values matter: they have to
# sit inside the distribution's support, or both engines throw and the
# comparison says nothing. Outcome first, then parameters.
DENSITIES = [
    ("std_normal_lpdf", 1, [0.4]),
    ("normal_lpdf", 3, [0.4, 0.2, 1.3]),
    ("lognormal_lpdf", 3, [1.4, 0.2, 1.3]),
    ("exponential_lpdf", 2, [1.4, 0.7]),
    ("cauchy_lpdf", 3, [0.4, 0.2, 1.3]),
    ("gamma_lpdf", 3, [1.4, 2.0, 1.3]),
    ("inv_gamma_lpdf", 3, [1.4, 2.0, 1.3]),
    ("beta_lpdf", 3, [0.4, 2.0, 1.3]),
    ("weibull_lpdf", 3, [1.4, 2.0, 1.3]),
    ("logistic_lpdf", 3, [0.4, 0.2, 1.3]),
    ("double_exponential_lpdf", 3, [0.4, 0.2, 1.3]),
    ("uniform_lpdf", 3, [0.4, -1.0, 1.5]),
    ("student_t_lpdf", 4, [0.4, 3.0, 0.2, 1.3]),
    # Not wired into the log-density path yet; --missing includes them and
    # the report becomes the to-do list.
    ("chi_square_lpdf", 2, [1.4, 3.0]),
    ("inv_chi_square_lpdf", 2, [1.4, 3.0]),
    ("scaled_inv_chi_square_lpdf", 3, [1.4, 3.0, 1.2]),
    ("frechet_lpdf", 3, [1.4, 2.0, 1.3]),
    ("gumbel_lpdf", 3, [0.4, 0.2, 1.3]),
    ("loglogistic_lpdf", 3, [1.4, 2.0, 1.3]),
    ("pareto_lpdf", 3, [2.4, 1.0, 1.3]),
    ("pareto_type_2_lpdf", 4, [1.4, 0.0, 1.2, 1.3]),
    ("rayleigh_lpdf", 2, [1.4, 1.3]),
    ("skew_normal_lpdf", 4, [0.4, 0.2, 1.3, 0.7]),
    ("von_mises_lpdf", 3, [0.4, 0.2, 1.3]),
    ("exp_mod_normal_lpdf", 4, [0.4, 0.2, 1.3, 0.7]),
    ("beta_proportion_lpdf", 3, [0.4, 0.6, 2.0]),
    ("skew_double_exponential_lpdf", 4, [0.4, 0.2, 1.3, 0.6]),
    # Discrete: an int outcome, written as a Python int so model_for emits
    # an integer literal and leaves slot 0 unperturbed -- there is no
    # derivative with respect to a count.
    ("poisson_lpmf", 2, [3, 1.3]),
    ("poisson_log_lpmf", 2, [3, 0.4]),
    ("bernoulli_lpmf", 2, [1, 0.4]),
    ("bernoulli_logit_lpmf", 2, [1, 0.4]),
    ("neg_binomial_lpmf", 3, [3, 2.0, 1.3]),
    ("neg_binomial_2_lpmf", 3, [3, 2.0, 1.3]),
    ("neg_binomial_2_log_lpmf", 3, [3, 0.4, 1.3]),
    ("beta_neg_binomial_lpmf", 4, [3, 2.0, 3.0, 1.3]),
    ("yule_simon_lpmf", 2, [3, 1.3]),
    # Two integer groups: the outcome and the trial count.
    ("beta_binomial_lpmf", 4, [3, 10, 2.0, 3.0]),
]

def cdf_specs(pool):
    """cdf/lcdf/lccdf entries derived from the lpdf rows above.

    A distribution function takes the same arguments as its density, and
    the arguments in that table are already chosen to sit in the support,
    so writing them out again would only create a second place to get them
    wrong. std_normal_cdf and friends take the outcome alone; everything
    else mirrors its density, lpmf included -- an integer outcome stays a
    Python int and model_for prints it as one. Only what stanli claims is
    emitted; the point is verifying what we ship.
    """
    # A tail function can be degenerate at an argument the density is
    # perfectly happy with. bernoulli's outcome 1 is the top of its
    # support, so bernoulli_lccdf(1 | theta) is log(0) BY DEFINITION --
    # stanli and CmdStan both return -inf with a zero gradient, and agree,
    # but stanli_check refuses to print a nonfinite lp while ref_driver
    # prints it, so the comparison reports a disagreement that is not one.
    # Move off the boundary rather than teach the harness to squint.
    OUTCOME_FOR_TAIL = {"bernoulli": 0}
    have = claimed()
    out = []
    for name, n, argv in pool:
        for suffix in ("_lpdf", "_lpmf"):
            if name.endswith(suffix):
                base = name[:-len(suffix)]
                break
        else:
            continue
        args = list(argv)
        if base in OUTCOME_FOR_TAIL:
            args[0] = OUTCOME_FOR_TAIL[base]
        for suffix in ("cdf", "lcdf", "lccdf"):
            fn = f"{base}_{suffix}"
            if fn in have:
                out.append((fn, n, args))
    return out


# Which of the above stanli claims. Anything here that fails is a bug;
# anything absent that passes is free coverage nobody wired up.
def claimed():
    """Names stanli wires into the log-density path.

    Two sources since the densities were generated: the hand-written
    entries still spelled out in lower.cpp, and the X-macro lists in
    optable.hpp, which generate the opcode, the kernel and the table entry
    from one line each.

    The list rows carry trailing fields that have changed over time (the
    instantiation tier arrived after this was first written), so match the
    name and the argument count and let the rest be anything. An
    over-tight pattern here fails silently: every density looks unclaimed,
    the sweep reports them as free coverage, and nothing is verified.
    """
    have = set(re.findall(r'"([a-z_0-9]+_(?:lpdf|lpmf))"',
                          (REPO / "runtime/src/lower.cpp").read_text()))
    have |= set(re.findall(r"X\(OP_[A-Z_0-9]+, ([a-z_0-9]+), \d+[,)]",
                           (REPO / "runtime/include/stanli/optable.hpp").read_text()))
    return have


MODEL = """data {{
  real y_data;
}}
parameters {{
  real p;
}}
model {{
  // Every argument is data except one, which carries the parameter, so
  // the gradient exercises the density's partial for that slot.
{body}
}}
"""


def model_for(name, argv, density=True):
    """Target sums the call once per differentiable slot, so the gradient
    exercises every partial the function has.

    A Python int in argv marks an integer slot: it prints without a
    decimal point (Stan would reject `3.0` where an int is wanted) and
    never takes the perturbation, since there is nothing to differentiate.
    """
    lines = []
    for k in range(len(argv)):
        if isinstance(argv[k], int):
            continue
        args = [f"{v}" for v in argv]
        args[k] = f"({args[k]} + p * 0.0625)"
        if density:
            lhs, rest = args[0], args[1:]
            call = (f"{name}({lhs} | {', '.join(rest)})" if rest
                    else f"{name}({lhs})")
        else:
            call = f"{name}({', '.join(args)})"
        lines.append(f"  target += {call};")
    return MODEL.format(body="\n".join(lines))


# Arguments by position for the generated scalar models. Most of the
# vocabulary is happy with these; a function whose domain excludes them
# fails on BOTH engines, which the comparison reports as such rather than
# as a difference.
SCALAR_ARGS = [0.5, 1.25, 0.75, 1.5, 2.0]


def from_stanc(stanc):
    """Every function with an all-real-scalar signature returning real."""
    dump = subprocess.run([str(stanc), "--dump-stan-math-signatures"],
                          capture_output=True, text=True).stdout
    out, seen = [], set()
    for line in dump.splitlines():
        m = re.match(r"([a-zA-Z_0-9]+)\((.*?)\) => (.*)", line)
        if not m:
            continue
        name, args, ret = m.group(1), m.group(2), m.group(3)
        if name in seen or ret != "real" or not args:
            continue
        parts = [a.strip() for a in args.split(",")]
        if any(p != "real" for p in parts) or len(parts) > len(SCALAR_ARGS):
            continue
        # The density and rng families have their own shapes and oracles.
        if name.endswith(("_lpdf", "_lpmf", "_rng", "_cdf", "_lcdf",
                          "_lccdf")):
            continue
        seen.add(name)
        out.append((name, len(parts), SCALAR_ARGS[:len(parts)]))
    return out


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def ulps(a, b):
    import struct
    if a == b:
        return 0
    ia = struct.unpack("<q", struct.pack("<d", a))[0]
    ib = struct.unpack("<q", struct.pack("<d", b))[0]
    if (ia < 0) != (ib < 0):
        return float("inf")
    return abs(ia - ib)


def sweep_one(spec, cs, tmp, keep, density=True):
    name, n, argv = spec
    d = tmp / name
    d.mkdir(parents=True, exist_ok=True)
    stan = d / f"{name}.stan"
    stan.write_text(model_for(name, argv[:n], density))
    data = d / "data.json"
    data.write_text(json.dumps({"y_data": 1.0}))

    ours = run([str(REPO / BUILD / "stanli_check"), str(stan), str(data),
                "--stanc", str(REPO / "deps/stanc3/stanc")])
    if "COMPILE_FAIL" in ours.stdout or "COMPILE_FAIL" in ours.stderr:
        why = (ours.stdout + ours.stderr).strip().splitlines()[-1]
        return name, "unsupported", why[:60], None, None

    exe, err = build_reference(cs, d, stan, REPO / "tools/ref_driver.cpp",
                               REPO / "deps/stanc3/stanc", name=name,
                               sundials=False, run=run, trim=60)
    if exe is None:
        status, _, note = err.partition(": ")
        return name, status, note, None, None

    worst, n_cmp, err = compare_points(exe, REPO / BUILD / "stanli_check",
                                       stan, data, ulps,
                                       REPO / "deps/stanc3/stanc", run=run)
    if err:
        return name, err[0], err[1], None, None
    if n_cmp == 0:
        return name, "no_valid_point", "", None, None

    ns = None
    sexp = d / "m.sexp"
    mir = run([str(REPO / "deps/stanc3/stanc"), "--O1", "--debug-optimized-mir", str(stan)])
    if mir.returncode == 0:
        sexp.write_text(mir.stdout)
        bench = run([str(REPO / BUILD / "bench_grad"), str(sexp), str(data), "20000"])
        if bench.returncode == 0 and bench.stdout.split():
            ns = float(bench.stdout.split()[0])
    if not keep:
        shutil.rmtree(d, ignore_errors=True)
    return name, "ok", "", worst, ns


def main():
    global BUILD
    ap = argparse.ArgumentParser()
    ap.add_argument("cmdstan", type=pathlib.Path)
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--missing", action="store_true")
    ap.add_argument("--from-stanc", action="store_true")
    ap.add_argument("--build", default=BUILD,
                    help="build tree holding stanli_check and bench_grad")
    args = ap.parse_args()
    BUILD = args.build

    have = claimed()
    density = not args.from_stanc
    pool = from_stanc(REPO / "deps/stanc3/stanc") if args.from_stanc else DENSITIES
    if not args.from_stanc:
        pool = pool + cdf_specs(pool)
    specs = [s for s in pool if args.filter in s[0]]
    if not args.missing and not args.from_stanc:
        specs = [s for s in specs if s[0] in have]

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_fnsweep_"))
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for r in ex.map(lambda s: sweep_one(s, args.cmdstan, tmp, args.keep,
                                           density),
                        specs):
            rows.append(r)
            print(".", end="", flush=True)
    print()

    print(f"{'function':<32} {'wired':<6} {'status':<16} {'ulp':>6} {'ns/grad':>10}")
    bad = 0
    for name, status, note, worst, ns in sorted(rows):
        wired = "yes" if name in have else "-"
        u = "" if worst is None else str(worst)
        t = "" if ns is None else f"{ns:,.0f}"
        print(f"{name:<32} {wired:<6} {status:<16} {u:>6} {t:>10}"
              + (f"  {note}" if note else ""))
        if name in have and status != "ok":
            bad += 1
        if status == "ok" and worst and worst > 2:
            bad += 1
    n_ok = sum(1 for r in rows if r[1] == "ok")
    print(f"\n{n_ok}/{len(rows)} evaluated and matched; "
          f"{sum(1 for r in rows if r[1] == 'unsupported')} unsupported")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
