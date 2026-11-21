#!/usr/bin/env python3
"""Corpus coverage harness: run stanli_check over the shared model/data
inventory and histogram the failures by missing feature.

Usage: tools/corpus.py PDB_DIR [--filter SUBSTR]
PDB_DIR is a posteriordb checkout containing posterior_database/.
Writes docs/corpus-status.md and prints the failure histogram.
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


# Context for models that evaluate but do not match, so the reason is not
# lost between runs.
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

    imported = [m for m in ok if m not in ver and m in refs]
    md = ["# Corpus status", "",
          f"Models that evaluate: {len(ok)}/{len(results)}",
          f"Models with CmdStan reference values: {sum(m in refs for m in results)}/{len(results)}, "
          f"at {sum(len(refs.get(m, {}).get('points', {})) for m in results)} evaluation points in total.", "",
          "The corpus includes models from posteriordb, generated brms and "
          "cogmod models, ctsem's state-space template, models from CRAN "
          "packages, imported teaching models, and language fixtures. The "
          "source collection column says where each model came from. "
          "`tools/verify_refs.py` checks all reference values the same way.", "",
          f"For {len(verified)} verified models, the table below shows how far "
          "stanli was from CmdStan at the primary evaluation point. These "
          "numbers were measured when the reference was recorded. Imported "
          "references keep their original CmdStan answers and a record of "
          "how each one was made. No deviation was recorded for them, so "
          "their rows say \"not recorded\". Having a reference "
          "for a model does not mean that the current build matches it. The "
          "numerical replay checks the current build.", "",
          "A model passes only when tools/verify_sample.py finds that "
          "stanli's log_prob and full gradient match CmdStan's at a shared, "
          "fixed evaluation point. The accuracy columns show the worst "
          "deviation over lp and every gradient component, as a relative "
          "difference and in ULPs. 0 ULP means bitwise identical to CmdStan. "
          "The bitwise results are for information only. The replay requires "
          "a scaled error of at most 1e-9, with documented exceptions for "
          "ill-conditioned models. Models that evaluate but are not verified "
          "are listed in a separate section and do not count as passing.",
          "",
          "| model | source collection | values compared | max rel diff | max ULP |",
          "| --- | --- | ---: | ---: | ---: |"]
    for m in sorted(verified + imported):
        if m in imported:
            pt = refs[m]["points"][str(refs[m]["primary"])]
            md.append(f"| `{m}` | {cases[m].collection} | {len(pt['values'])} | not recorded | not recorded |")
        else:
            v = ver[m]
            rel = "0 (bitwise)" if v["max_rel"] == 0 else f"{v['max_rel']:.1e}"
            md.append(f"| `{m}` | {cases[m].collection} | {v['n_values']} | {rel} | {v['max_ulp']} |")
    noted = [m for m in verified if m in NOTES]
    if noted:
        md += ["", "Numerical notes:", ""]
        for m in noted:
            points = refs.get(m, {}).get("points", {}).values()
            deviations = [p["max_ulp"] for p in points if "max_ulp" in p]
            measured = (f" Worst recorded deviation across all points: "
                        f"{max(deviations)} ULP." if deviations else "")
            md.append(f"- `{m}`: {NOTES[m]}{measured}")
    wa_refs = {}
    if REFS_PATH.exists():
        # Every point carries its own write_array reference; the table
        # counts the primary point's, the one this scoreboard's other
        # columns already describe.
        refs = load_refs()[0]
        for m, v in refs.items():
            pt = v["points"].get(str(v.get("primary")), {})
            if "wa" in pt and m in results:
                wa_refs[m] = len(pt["wa"]["values"])
    if wa_refs:
        md += ["", "## write_array references", "",
               "The reference recorder also saves CmdStan's write_array "
               "output at the same point. This covers every CSV column: "
               "constrained parameters, transformed parameters and generated "
               "quantities. Both drivers call write_array directly and use "
               "Stan's RNG with the same seed and chain 0, so the "
               "generated-quantity draws can be compared too. In CI, "
               "tools/verify_refs.py replays these rows. Column names must "
               "match exactly, and values must pass the same tolerance as the "
               "model's log density and gradient.", "",
               "| model | write_array values compared |",
               "| --- | ---: |"]
        for m in sorted(wa_refs):
            md.append(f"| `{m}` | {wa_refs[m]} |")
    rejected = [m for m, v in ver.items()
                if v.get("status") == "REJECTED_BOTH"]
    if rejected:
        md += ["", "## Rejected by both engines", "",
               "CmdStan and stanli both reject every shared evaluation "
               "point for these models. The model is invalid at those "
               "points, for example when an ODE solution dips below a "
               "declared lower bound. There is nothing to compare. The two "
               "engines agree, but these models do not count as verified.", ""]
        md += [f"- `{m}`" for m in sorted(rejected)]
    unver = [m for m in ok if m not in verified and m not in rejected and m not in imported]
    if unver:
        md += ["", "## Evaluate but not verified", ""]
        for m in unver:
            v = ver.get(m)
            why = (f"max rel diff {v['max_rel']:.1e}" if v
                   else "not yet run through verify_sample.py")
            md.append(f"- `{m}`: {why}")
            if m in NOTES:
                md.append(f"  - {NOTES[m]}")
    md += ["", "## Failures", ""]
    for model, (s, msg) in sorted(results.items()):
        if s != "OK":
            md.append(f"- `{model}`: {s} {msg}")
    (REPO / "docs" / "corpus-status.md").write_text("\n".join(md) + "\n",
                                                    encoding="utf-8")


if __name__ == "__main__":
    main()
