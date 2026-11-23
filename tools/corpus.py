#!/usr/bin/env python3
"""Corpus coverage harness: run stanli_check over the shared model/data
inventory and histogram the failures by missing feature.

Usage: tools/corpus.py PDB_DIR [--filter SUBSTR]
PDB_DIR is a posteriordb checkout containing posterior_database/.
Prints which models evaluate and the failure histogram. The per-model
limits and the reference comparison are in TESTING.md, which
tools/gen_docs.py generates from the references and the ULP ledger.
"""
import collections
import json
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from corpus_inventory import corpus_cases, materialize_data  # noqa: E402
from verify_refs import (REFS_PATH, default_check_bin, load_refs,
                         parse_status)  # noqa: E402

CHECK = default_check_bin()

# Verification results written by tools/verify_sample.py. A model counts as
# passing only if it appears here as VERIFIED; compiling and returning a
# finite gradient is never sufficient.
VERIFY_JSON = REPO / "docs" / "internal" / "artifacts" / "verification.json"


# Why these models differ from CmdStan. The ULP ledger's reasons and
# MISMATCH_OUTPUTS in tools/verify_refs.py refer to these.
NOTES = {
    "dogs":
        "CmdStan sums Bernoulli terms one call per iteration; stanli's "
        "merged call uses Eigen reductions and vectorized exp/log1p. "
        "The different reduction order can change final rounding in the "
        "log density and gradient.",
    "dogs_log":
        "As for dogs, merging Bernoulli terms changes their reduction "
        "order and can change final rounding. The primary point need "
        "not have the largest deviation of the three probes.",
    "examplemodels_gp_fit_multi_output":
        "The covariance of 101 points 0.04 apart with a 1e-9 jitter has "
        "condition number 5.6e10 at point 0, with 89 of its 101 eigenvalues "
        "at the jitter. Against an 80-digit evaluation at point 0 the log "
        "density is 7.3e-9 off on macOS arm64, in stanli and CmdStan alike, "
        "and the gradient of log(rho) is 6.6e-8 off; Linux x86-64 measured "
        "that gradient 3.9e-7 from the arm64 recording. Moving log(rho) by "
        "2.4e-15 moves the arm64 log density by up to 1.1e-8 and the "
        "gradient by up to 6.2e-7.",
    "kronecker_gp":
        "lp matches CmdStan to 1e-13 and 436/438 gradients match; the two "
        "that flow through eigenvectors_sym differ by 0.7%. The covariance "
        "at this data has 8 of 29 eigenvalue gaps below 1e-12 (smallest "
        "6.5e-17), and eigenvector derivatives scale as 1/(lambda_i - "
        "lambda_j), so last-bit differences in the input are amplified by "
        "~1e16. Every component op (eigen decomposition, transpose, matrix "
        "product, the whole chain with one operand held constant) matches "
        "CmdStan bitwise in isolation.",
    "stanguide_ode_adjoint":
        "With the guide's adjoint call and tolerances of 1e-6, stanli and "
        "CmdStan differ by 3.9e-6 and 3.3e-6 relative at the first two "
        "points and agree to 3.8e-14 at the third. With every tolerance at "
        "1e-11 the two agree to 5e-11 and 9e-11 at the first two points. "
        "Against that tight-tolerance solution at the first point, the log "
        "density is off by 3.0e-6 in both engines, the first gradient entry "
        "by 4.0e-6 in stanli and 1.5e-5 in CmdStan, and the second by 4.0e-5 "
        "in stanli and 4.4e-5 in CmdStan (absolute differences).",
    "tuple_array_params":
        "At the third point the gradient of grid[1, 2].1's free value is "
        "1 - x - x^2 * sum(v^2) / 9 with x = 1 and v = (1/3, 1/3, 1/3), "
        "which cancels to -1/27. Its terms are summed in a different "
        "order: evaluated exactly at the same inputs, stanli is 8 ulp from "
        "the result and CmdStan 24 ulp, 32 ulp apart.",
}


def load_verification():
    if not VERIFY_JSON.exists():
        return {}
    return json.loads(VERIFY_JSON.read_text())


def main():
    pdb = pathlib.Path(sys.argv[1]) / "posterior_database"
    filt = sys.argv[sys.argv.index("--filter") + 1] if "--filter" in sys.argv else ""
    cases = corpus_cases(pdb, include_language=True)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_corpus_"))

    results = {}
    reasons = collections.Counter()
    for model, case in cases.items():
        if filt and filt not in model:
            continue
        stan = case.source
        if not stan.exists() or not case.data.exists():
            results[model] = ("SKIP", "missing files")
            continue
        dj = materialize_data(case, tmp)
        try:
            # Same point walk as verify_sample/ref_driver: a model can be
            # legitimately out of support at one probe point (dogs_log's
            # uniform priors reject point 0) and fine at the next.
            for point in ("0", "1", "2"):
                out = subprocess.run(
                    [str(CHECK), str(stan), str(dj), "--point", point],
                    capture_output=True,
                    text=True, timeout=120, cwd=REPO).stdout
                status_fields = parse_status(out)
                if status_fields[:1] == ["OK"]:
                    break
        except subprocess.TimeoutExpired:
            out = "EVAL_FAIL timeout"
            status_fields = ["EVAL_FAIL", "timeout"]
        if status_fields[:1] == ["OK"]:
            results[model] = ("OK", "")
        else:
            status = status_fields[0] if status_fields else "CRASH"
            msg = " ".join(status_fields[1:]) if status_fields else out.strip()
            results[model] = (status, msg)
            # Classify by the interesting token.
            key = msg
            if "For loops" in msg:
                key = "For loops"
            elif "IfElse" in msg:
                key = "IfElse"
            elif "indexed assignment" in msg:
                key = "indexed assignment"
            else:
                m = re.search(r"unsupported (?:function |statement function )?([\w]+)", msg)
                if m:
                    key = f"unsupported {m.group(1)}"
            reasons[key] += 1

    refs = load_refs()[0] if REFS_PATH.exists() else {}
    ver = load_verification()
    ver = {m: v for m, v in ver.items() if m in results}
    ok = sorted(m for m, (s, _) in results.items() if s == "OK")
    verified = [m for m in ok
                if ver.get(m, {}).get("status") == "VERIFIED"]
    print(f"\n== {len(ok)}/{len(results)} models evaluate "
          f"({len(verified)} verified vs CmdStan) ==")
    for m in ok:
        tag = "OK      " if m in verified else "EVAL-ONLY"
        print(f"  {tag} {m}")
    print("\n== failure histogram ==")
    for k, c in reasons.most_common(30):
        print(f"  {c:3d}  {k}")

    rejected = {m for m, v in ver.items() if v.get("status") == "REJECTED_BOTH"}
    unverified = [m for m in ok if m not in verified and m not in rejected
                  and m not in refs]
    if unverified:
        print("\n== evaluate, no CmdStan reference ==")
        for m in unverified:
            print(f"  {m}")
    failed = {m: r for m, r in sorted(results.items()) if r[0] != "OK"}
    if failed:
        print("\n== failures ==")
        for m, (status, msg) in failed.items():
            print(f"  {m}: {status} {msg}")
            if m in NOTES:
                print(f"    {NOTES[m]}")


if __name__ == "__main__":
    main()
