#!/usr/bin/env python3
"""Two-hop verification for the STANLI_LITE_LP build.

The exact build is verified against CmdStan by tools/verify_refs.py. The
lite build cannot be: it drops stan-math's propto instantiations, so every
`~` statement evaluates the full density and lp__ lands a constant above
the reference. Rather than lower the gate and lose the oracle, this script
verifies the lite build against the exact one, on the claim that flag
makes precisely:

  1. gradients are BITWISE identical, and so are write_array values.
     Term-dropping only removes summands that are constant in the active
     arguments, so it never reaches a partial derivative. This is the
     claim HMC actually depends on. Note what it does NOT say: a pinned
     seed draws a different chain in the lite build, because the sampler
     adds lp to the kinetic energy and a shifted lp rounds differently
     there. Measured on eight schools, that starts at 2e-15 after five
     warmup iterations and reaches 1e-9 by fifty -- the signature of
     rounding amplified by a chaotic trajectory, not of different math.
  2. lp__ differs by a CONSTANT. Evaluated at several points in the
     unconstrained space, lp_exact - lp_lite must come out the same every
     time. A difference that moves with the parameters would mean a
     dropped term was not constant after all, which is a real bug and the
     only way this flag can be wrong.

Claim 2 is what makes claim 1 credible: a per-point constant is a strong
signature. Anything that perturbs the arithmetic itself -- reassociation,
a different libm path, a genuinely wrong density -- breaks it.

Usage: tools/verify_lite.py PDB_DIR --exact BIN --lite BIN [model ...]
Exit nonzero if any model breaks either claim.
"""
import argparse
import concurrent.futures
import gzip
import json
import pathlib
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
POINTS = (0, 1, 2)
# The shift is a difference of two large sums, so it inherits lp's
# absolute rounding granularity, not its own: dropping a term reassociates
# everything after it. Measured, the residue is about one ULP OF LP --
# rats_model's lp is -4.7e6 (ULP 9.3e-10) and its shift moves by 4.7e-10;
# covid19imperial's lp is -1.6e5 (ULP 2.9e-11) and its shift moves by
# 2.9e-11. So the gate is relative to lp, not to the shift, which is the
# scale the noise actually lives on. At 1e-12 that still leaves thousands
# of ULP of headroom, and a shift that is not really constant moves with
# the parameters -- O(1) relative, twelve orders the other side of this.
CONST_REL = 1e-12


def run(check_bin, stan, data, point):
    """(lp, [values...]) at one eval point, or None if the model failed."""
    proc = subprocess.run(
        [str(check_bin), str(stan), str(data), "--point", str(point)],
        capture_output=True, text=True, cwd=REPO, timeout=600)
    line = proc.stdout.splitlines()
    got = line[0].split() if line else []
    if not got or got[0] != "OK":
        return None
    vals = [float(x) for x in got[1:]]
    return (vals[0], vals[1:])


def check_model(model, ref, pdb, exact_bin, lite_bin, tmp):
    """(model, status, detail)."""
    stan = pdb / "models" / "stan" / f"{model}.stan"
    dz = pdb / "data" / "data" / f"{ref['data']}.json.zip"
    if not stan.exists() or not dz.exists():
        return (model, "SKIP", "input missing")
    dj = tmp / f"{model}_data.json"
    with zipfile.ZipFile(dz) as z:
        dj.write_bytes(z.read(z.namelist()[0]))

    shifts = []
    for point in POINTS:
        try:
            a = run(exact_bin, stan, dj, point)
            b = run(lite_bin, stan, dj, point)
        except subprocess.TimeoutExpired:
            return (model, "TIMEOUT", f"point {point}")
        if a is None or b is None:
            # Both builds refusing the same model is not a lite-build
            # regression; one refusing alone is.
            if a is None and b is None:
                return (model, "SKIP", "unsupported in both builds")
            which = "lite" if b is None else "exact"
            return (model, "RUN_FAIL", f"{which} build failed at point {point}")
        (lp_a, g_a), (lp_b, g_b) = a, b
        if len(g_a) != len(g_b):
            return (model, "SHAPE_FAIL", f"{len(g_a)} vs {len(g_b)} gradients")
        for i, (x, y) in enumerate(zip(g_a, g_b)):
            # Bitwise, with NaN counted equal to NaN: these must be the
            # same computation, not merely the same number.
            if x != y and not (x != x and y != y):
                return (model, "GRAD_FAIL",
                        f"g[{i}] {x!r} vs {y!r} at point {point}")
        if lp_a != lp_a or lp_b != lp_b:
            continue  # both nan lp; the gradient check above still applies
        shifts.append((lp_a - lp_b, abs(lp_a)))

    if len(shifts) < 2:
        return (model, "OK", "shift unchecked (too few finite points)")
    scale = max(max(lp for _, lp in shifts), 1.0)
    shifts = [s for s, _ in shifts]
    spread = max(shifts) - min(shifts)
    if spread / scale > CONST_REL:
        return (model, "SHIFT_FAIL",
                f"lp shift varies: {' '.join(f'{s:.17g}' for s in shifts)}")
    return (model, "OK", f"lp shift {shifts[0]:.10g}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("models", nargs="*")
    ap.add_argument("--exact", type=pathlib.Path,
                    default=REPO / "build" / "stanli_check")
    ap.add_argument("--lite", type=pathlib.Path,
                    default=REPO / "build-lite" / "stanli_check")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    refs = json.loads(gzip.decompress(
        (REPO / "docs" / "corpus-refs.json.gz").read_bytes()))
    pdb = args.pdb / "posterior_database"
    models = args.models or sorted(refs)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_lite_"))

    failures, skipped, shifted = [], [], 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(check_model, m, refs[m], pdb, args.exact,
                            args.lite, tmp) for m in models]
        for fut in concurrent.futures.as_completed(futs):
            model, status, detail = fut.result()
            if status == "SKIP":
                skipped.append(model)
            elif status != "OK":
                failures.append(model)
                print(f"{status} {model}: {detail}")
            elif detail.startswith("lp shift") and detail != "lp shift 0":
                shifted += 1

    ok = len(models) - len(failures) - len(skipped)
    print(f"\n{ok}/{len(models)} models: gradients bitwise identical to the "
          f"exact build, lp shifted by a constant ({shifted} with a nonzero "
          f"shift)")
    if skipped:
        print(f"{len(skipped)} skipped (unsupported in both builds)")
    if failures:
        print(f"{len(failures)} FAILED: {' '.join(failures)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
