#!/usr/bin/env python3
"""The one recipe for a CmdStan reference binary, and the comparison loop.

stanc emits the model header, clang++ compiles it against the CmdStan
checkout's stan-math and links tbb, plus CVODES when the model can reach
the stiff ODE solvers. Every verification harness and benchmark rig in
the tree needs exactly this; what they vary is the driver they compile
in, the optimization level, whether they need sundials and how they run
subprocesses, so those stay per-caller parameters.

-ffp-contract=off matches CmdStan's own build (stan-math's makefiles set
it); without it the reference binary forms FMAs and drifts a few ULP from
what CmdStan actually computes, which is the whole point of the binary.
"""
import subprocess

SUNDIALS_LIBS = ("cvodes", "idas", "kinsol", "nvecserial")


def includes(cs):
    """The -I paths a stanc-generated model header needs."""
    math = cs / "stan" / "lib" / "stan_math"
    return [cs / "stan" / "src", math,
            next((cs / "stan" / "lib").glob("rapidjson_*")),
            next((math / "lib").glob("eigen_*")),
            next((math / "lib").glob("boost_*")),
            next((math / "lib").glob("sundials_*")) / "include",
            next((math / "lib").glob("tbb_*")) / "include"]


def compile_cmd(cs, hpp, driver, exe, opt="-O1", sundials=True):
    """clang++ argv compiling `driver` against the model header `hpp`."""
    math = cs / "stan" / "lib" / "stan_math"
    tbb = math / "lib" / "tbb"
    cmd = ["clang++", "-std=c++17", opt, "-ffp-contract=off", "-D_REENTRANT",
           "-DBOOST_DISABLE_ASSERTS"]
    cmd += [f"-I{i}" for i in includes(cs)]
    cmd += ["-include", str(hpp), str(driver),
            f"-L{tbb}", "-ltbb", f"-Wl,-rpath,{tbb}"]
    if sundials:
        # rk45 is header-only Boost odeint, but bdf/adams reach CVODES:
        # without these the link fails on those models and the row reads
        # as a coverage gap rather than a missing -l.
        sun = next((math / "lib").glob("sundials_*")) / "lib"
        cmd += [str(sun / f"libsundials_{n}.a") for n in SUNDIALS_LIBS]
    cmd += ["-o", str(exe)]
    return cmd


def _plain(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def build_reference(cs, workdir, stan, driver, stanc, name="ref", opt="-O1",
                    sundials=True, run=_plain, trim=90):
    """stanc + compile in workdir; returns (exe, None) or (None, reason)."""
    hpp = workdir / f"{name}.hpp"
    if run([str(stanc), str(stan), f"--o={hpp}"]).returncode != 0:
        return None, "stanc_fail"
    exe = workdir / "ref"
    b = run(compile_cmd(cs, hpp, driver, exe, opt, sundials))
    if b.returncode != 0:
        return None, "ref_build_fail: " + b.stderr.strip()[-trim:]
    return exe, None


def compare_points(ref_exe, check_bin, stan, data, metric, stanc, n=3,
                   run=_plain):
    """Compare reference and stanli at n deterministic points.

    Returns (worst, n_compared, error), where error is None or a
    (kind, detail) pair the caller formats its own way.

    Both engines rejecting a point is agreement (the point is outside the
    support); one rejecting is a real disagreement. The two spell the
    rejection differently, and reading that as disagreement would be
    wrong: stanli_check reports EVAL_FAIL and prints no OK line, while
    CmdStan's log_prob returns a row of nan and only throws later, out of
    write_array. A unit_vector at the origin is exactly this. So an
    all-nan reference row counts as a rejection.
    """
    worst, n_cmp = 0, 0
    for point in range(n):
        ref = run([str(ref_exe), str(data), str(point)])
        got = run([str(check_bin), str(stan), str(data), "--stanc",
                   str(stanc), "--point", str(point)])
        rf = [l for l in ref.stdout.splitlines() if l.startswith("OK")]
        gf = [l for l in got.stdout.splitlines() if l.startswith("OK")]
        if rf and all(x != x for x in (float(v) for v in rf[0].split()[1:])):
            rf = []
        if not rf or not gf:
            if bool(rf) != bool(gf):
                return worst, n_cmp, ("one_side_threw", f"point {point}")
            continue
        a = [float(x) for x in rf[0].split()[1:]]
        b = [float(x) for x in gf[0].split()[1:]]
        if len(a) != len(b):
            return (worst, n_cmp,
                    ("shape_mismatch", f"cmdstan {len(a)}, stanli {len(b)}"))
        n_cmp += len(a)
        worst = max([worst] + [metric(x, y) for x, y in zip(a, b)])
    return worst, n_cmp, None
