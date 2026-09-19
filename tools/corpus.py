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
VERIFY_JSON = REPO / "docs" / "verification.json"


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
    "kronecker_gp":
        "lp matches CmdStan to 1e-13 and 436/438 gradients match; the two "
        "that flow through eigenvectors_sym differ by 0.7%. The covariance "
        "at this data has 8 of 29 eigenvalue gaps below 1e-12 (smallest "
        "6.5e-17), and eigenvector derivatives scale as 1/(lambda_i - "
        "lambda_j), so last-bit differences in the input are amplified by "
        "~1e16. Every component op (eigen decomposition, transpose, matrix "
        "product, the whole chain with one operand held constant) matches "
        "CmdStan bitwise in isolation.",
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
          f"Evaluating: {len(ok)}/{len(results)}",
          f"CmdStan reference coverage: {sum(m in refs for m in results)}/{len(results)} models, "
          f"{sum(len(refs.get(m, {}).get('points', {})) for m in results)} evaluation points.", "",
          "The shared corpus includes posteriordb, generated brms models, "
          "imported teaching models, and language fixtures. Collection labels "
          "retain their source provenance; all references use the same replay "
          "in `tools/verify_refs.py`.", "",
          f"Recording-time primary-point comparison metrics retained for {len(verified)} "
          "verified models are shown below. Imported references retain their original "
          "answers and per-model recording provenance, without inventing historical "
          "comparison metrics. Reference coverage is separate from a current-build "
          "numerical replay result.", "",
          "A model counts as passing only when tools/verify_sample.py "
          "matches CmdStan's log_prob and full gradient at the shared "
          "deterministic point. Accuracy below is the worst deviation "
          "over lp and every gradient component: relative, and in ULPs "
          "(0 = bitwise identical to CmdStan). Bitwise counts are "
          "reported for information; the replay uses a 1e-9 scaled-error "
          "gate with documented ill-conditioned exceptions. Models that evaluate but are not verified are listed "
          "separately and are not counted.",
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
               "The oracle also records CmdStan's write_array at the same "
               "point: every CSV column (constrained parameters, transformed "
               "parameters, generated quantities). Both direct-write-array "
               "drivers use Stan's RNG with the same seed and chain 0, so "
               "generated-quantity draws are compared too. "
               "tools/verify_refs.py replays the rows in CI "
               "with column names matched exactly and values sharing the "
               "model's gate.", "",
               "| model | write_array values compared |",
               "| --- | ---: |"]
        for m in sorted(wa_refs):
            md.append(f"| `{m}` | {wa_refs[m]} |")
    rejected = [m for m, v in ver.items()
                if v.get("status") == "REJECTED_BOTH"]
    if rejected:
        md += ["", "## Rejected by both engines", "",
               "CmdStan and stanli both reject every shared evaluation "
               "point for these models: the model is invalid there (an ODE "
               "solution dipping below a declared lower bound, for "
               "instance), so there is nothing to compare. Agreement, not "
               "a gap, but not counted as verified either.", ""]
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
    (REPO / "docs" / "corpus-status.md").write_text("\n".join(md) + "\n")


if __name__ == "__main__":
    main()
