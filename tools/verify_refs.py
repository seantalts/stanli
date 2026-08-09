#!/usr/bin/env python3
"""Differential corpus verification against committed CmdStan references.

tools/verify_sample.py runs CmdStan itself and records the exact lp and
gradient values it printed into docs/corpus-refs.json.gz. This script
replays stanli against those recorded values, which needs no CmdStan, no
C++ toolchain, and no 2 GB checkout: just a stanli_check binary and the
posteriordb model + data files. That is what lets the strongest oracle in
the project run in CI on every push, on every platform.

The references were generated on macOS arm64 with Apple's libm. Other
platforms' libm implementations round transcendentals differently, so the
gate here is deliberately looser than the 1e-10 the generating rig holds
itself to: 1e-9 relative. Every bug class that has actually reached the
corpus (silent in-place corruption at 1.7e+05 relative, quadratic
recompute, dropped tape links) sits many orders of magnitude above it,
and honest cross-libm drift sits well below.

Usage: tools/verify_refs.py PDB_DIR [--check BIN] [--max-rel X]
                            [--jobs N] [--timeout S] [model ...]
Exit nonzero if any referenced model fails to run, changes shape, or
exceeds the gate.
"""
import argparse
import concurrent.futures
import gzip
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent


def ulp_distance(a, b):
    if a == b:
        return 0
    ia, ib = (struct.unpack("<q", struct.pack("<d", v))[0] for v in (a, b))
    key = lambda i: (-(1 << 63)) - i if i < 0 else i
    return abs(key(ia) - key(ib))


def pair_dev(a, b):
    """(rel, ulp) for one reference/stanli pair, nonfinite-safe.

    Both NaN, or the same infinity, is agreement (0, 0). A nonfinite
    value on one side only is an infinite relative deviation: the old
    arithmetic produced NaN here and Python's max() silently kept the
    running value, which hid dogs_log disagreeing with CmdStan at -inf
    for months.
    """
    if a != a and b != b:
        return (0.0, 0)
    if a == b:
        return (0.0, 0)
    if not (a - a == 0.0 and b - b == 0.0):
        return (float("inf"), ulp_distance(a, b) if a == a and b == b else 0)
    scale = max(abs(a), abs(b), 1.0)
    return (abs(a - b) / scale, ulp_distance(a, b))


def parse_wa(out):
    """(names_csv, value_strings) from WANAMES/WAVALS lines, or None."""
    names, vals = None, None
    for line in out.splitlines():
        if line.startswith("WANAMES "):
            names = line[8:]
        elif line.startswith("WAVALS"):
            vals = line[6:].split()
    if names is None or vals is None or names.startswith("FAIL"):
        return None
    if vals and vals[0] == "FAIL":
        return None
    return (names, vals)


def check_model(model, ref, pdb, check_bin, tmp, timeout, no_wa=False,
                no_lp=False):
    """Returns (model, status, max_rel, max_ulp, n_values, detail)."""
    stan = pdb / "models" / "stan" / f"{model}.stan"
    dz = pdb / "data" / "data" / f"{ref['data']}.json.zip"
    if not stan.exists() or not dz.exists():
        return (model, "MISSING_INPUT", 0.0, 0, 0, str(stan))
    dj = tmp / f"{model}_data.json"
    with zipfile.ZipFile(dz) as z:
        dj.write_bytes(z.read(z.namelist()[0]))
    cmd = [str(check_bin), str(stan), str(dj), "--point", str(ref["point"])]
    if "wa" in ref and not no_wa:
        cmd.append("--wa-values")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return (model, "TIMEOUT", 0.0, 0, 0, "")
    lines = proc.stdout.splitlines()
    got = lines[0].split() if lines else []
    if not got or got[0] != "OK":
        # Say HOW it died, not just that it did: a negative returncode is
        # a signal (ldaK5's 49 GB compile came back as a bare RUN_FAIL
        # because the OOM killer leaves no stdout).
        detail = " ".join(got[:3])
        if not detail:
            detail = (f"killed by signal {-proc.returncode}"
                      if proc.returncode < 0
                      else f"exit {proc.returncode}, no output")
        err = proc.stderr.strip().splitlines()
        if err:
            detail += " | " + err[-1][:120]
        return (model, "RUN_FAIL", 0.0, 0, 0, detail)
    rv = [float(x) for x in ref["values"]]
    gv = [float(x) for x in got[1:]]
    if len(rv) != len(gv):
        return (model, "SHAPE_FAIL", 0.0, 0, 0, f"{len(rv)} vs {len(gv)}")
    if no_lp:
        # Element 0 is lp. A STANLI_LITE_LP build computes the full
        # density where CmdStan's `~` drops constant terms, so its lp sits
        # a per-model constant above the reference while every gradient is
        # unchanged -- which is the part sampling depends on. Dropping the
        # element keeps the corpus oracle usable for that build. Whether
        # the offset is genuinely CONSTANT is a different question, and
        # the one that can actually catch a bug; tools/verify_lite.py
        # answers it by evaluating both builds at several points.
        rv, gv = rv[1:], gv[1:]
    worst, worst_ulp = 0.0, 0
    for a, b in zip(rv, gv):
        rel, ulp = pair_dev(a, b)
        worst = max(worst, rel)
        worst_ulp = max(worst_ulp, ulp)
    n = len(rv)
    if "wa" in ref and not no_wa:
        # The write_array reference: column names must match exactly, and
        # the values (transformed parameters + generated quantities at the
        # same point) share the model's gate.
        names, vals = None, None
        for line in lines[1:]:
            if line.startswith("WANAMES "):
                names = line[8:]
            elif line.startswith("WAVALS"):
                vals = line[6:].split()
        if names is None or vals is None or names.startswith("FAIL"):
            return (model, "WA_FAIL", worst, worst_ulp, n,
                    (names or "no write_array output")[:120])
        if names != ref["wa"]["names"]:
            return (model, "WA_NAMES_FAIL", worst, worst_ulp, n,
                    f"got {names[:60]} want {ref['wa']['names'][:60]}")
        wref = [float(x) for x in ref["wa"]["values"]]
        wgot = [float(x) for x in vals]
        if len(wref) != len(wgot):
            return (model, "WA_SHAPE_FAIL", worst, worst_ulp, n,
                    f"{len(wref)} vs {len(wgot)}")
        for a, b in zip(wref, wgot):
            rel, ulp = pair_dev(a, b)
            worst = max(worst, rel)
            worst_ulp = max(worst_ulp, ulp)
        n += len(wref)
    return (model, "OK", worst, worst_ulp, n, "")


def gate_for(ref, default):
    """The threshold this model is held to.

    A model recorded as MISMATCH is one whose disagreement with CmdStan is
    documented and understood (kronecker_gp: two of 438 gradients flow
    through eigenvectors of a nearly degenerate covariance). Gating it at
    the clean threshold would fail every run; ignoring it would let a real
    regression hide behind a known deviation. Gate it above what it was
    recorded at, so it can never get much worse unnoticed.

    4x, not 2x: an ill-conditioned eigendecomposition amplifies ISA-level
    differences, and the deviation itself moves across platforms. Measured
    for kronecker_gp: 7.1e-3 on arm64 (where the reference was recorded),
    1.71e-2 on both x86_64 runners, identical to each other. The gate is a
    tripwire for the regression class this corpus has actually caught,
    which measured 1.7e+5 relative, seven orders of magnitude above it.
    """
    if ref.get("status") == "MISMATCH":
        return max(ref.get("max_rel", 0.0) * 4.0, default)
    return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("models", nargs="*")
    ap.add_argument("--check", type=pathlib.Path,
                    default=REPO / "build" / "stanli_check")
    ap.add_argument("--max-rel", type=float, default=1e-9)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--no-lp", action="store_true",
                    help="compare gradients only, not lp: a STANLI_LITE_LP "
                         "build shifts lp by a constant (docs/lite-lp.md)")
    ap.add_argument("--no-wa", action="store_true",
                    help="replay lp and gradients only (the WASM check "
                         "driver has no write_array entry point yet)")
    ap.add_argument("--skip", default="",
                    help="comma-separated models to exclude (the wasm32 "
                         "build cannot fit nn_rbm1bJ100's compile in 4GB)")
    ap.add_argument("--timeout", type=float, default=300)
    args = ap.parse_args()

    refs = json.loads(gzip.decompress(
        (REPO / "docs" / "corpus-refs.json.gz").read_bytes()))
    pdb = args.pdb / "posterior_database"
    models = args.models or sorted(refs)
    skip = set(filter(None, args.skip.split(",")))
    models = [m for m in models if m not in skip]
    missing = [m for m in models if m not in refs]
    if missing:
        print(f"no reference recorded for: {' '.join(missing)}")
        return 2

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_refs_"))
    failures = []
    worst_overall = ("", 0.0, 0)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(check_model, m, refs[m], pdb, args.check, tmp,
                            args.timeout, args.no_wa, args.no_lp)
                for m in models]
        for fut in concurrent.futures.as_completed(futs):
            model, status, rel, ulp, n, detail = fut.result()
            if status != "OK":
                failures.append((model, status, detail))
                print(f"{status} {model} {detail}")
                continue
            if (rel > worst_overall[1]
                    and refs[model].get("status") != "MISMATCH"):
                worst_overall = (model, rel, ulp)
            gate = gate_for(refs[model], args.max_rel)
            if rel >= gate:
                failures.append((model, f"rel {rel:.2e}", f"{ulp} ulp"))
                print(f"GATE {model}: {rel:.2e} ({ulp} ulp) over {n} "
                      f"values, allowed {gate:.1e}")

    ok = len(models) - len(failures)
    print(f"\n{ok}/{len(models)} models within {args.max_rel:.0e} of the "
          f"CmdStan references"
          + (" (gradients only)" if args.no_lp else "")
          + (f"; worst {worst_overall[0]} at {worst_overall[1]:.2e} "
             f"({worst_overall[2]} ulp)" if worst_overall[0] else ""))
    if failures:
        print(f"{len(failures)} FAILED: "
              + " ".join(m for m, _, _ in failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
