#!/usr/bin/env python3
"""Corpus coverage harness: run stanli_check over every posteriordb
(model, dataset) pair and histogram the failures by missing feature.

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
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from verify_refs import (LOCAL_CORPORA, REFS_PATH,  # noqa: E402
                         default_check_bin, load_refs, parse_status)

CHECK = default_check_bin()

# Verification results written by tools/verify_sample.py. A model counts as
# passing only if it appears here as VERIFIED; compiling and returning a
# finite gradient is never sufficient.
VERIFY_JSON = REPO / "docs" / "verification.json"


# Context for models that evaluate but do not match, so the reason is not
# lost between runs.
NOTES = {
    "dogs":
        "31 and 32 ULP from CmdStan at two of the three recorded points, "
        "against a 30 ULP budget. Against a 60-digit reference both engines "
        "are off by about as much: CmdStan sums the 750 Bernoulli terms one "
        "at a time and lands 10 to 59 ULP from the true log density, stanli's "
        "one merged call uses Eigen's packet reduction and vectorized "
        "exp/log1p and lands 15 to 63 ULP off, and on the gradient each "
        "engine is the closer one at a different point. Matching CmdStan "
        "would mean adopting its order; pairwise summation would put the "
        "merged call within 1 ULP of the reference at a larger distance from "
        "CmdStan.",
    "dogs_log":
        "bitwise at the primary point and 25 ULP at another recorded "
        "point, for the same reason as dogs, inside the 30 ULP budget.",
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
    posteriors = sorted((pdb / "posteriors").glob("*.json"))
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_corpus_"))

    seen_models = set()
    results = {}
    reasons = collections.Counter()
    for pj in posteriors:
        meta = json.loads(pj.read_text())
        model = meta["model_name"]
        if model in seen_models or (filt and filt not in model):
            continue
        seen_models.add(model)
        stan = pdb / "models" / "stan" / f"{model}.stan"
        dz = pdb / "data" / "data" / f"{meta['data_name']}.json.zip"
        if not stan.exists() or not dz.exists():
            results[model] = ("SKIP", "missing files")
            continue
        dj = tmp / f"{meta['data_name']}.json"
        if not dj.exists():
            with zipfile.ZipFile(dz) as z:
                dj.write_bytes(z.read(z.namelist()[0]))
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

    ver = load_verification()
    # The models carried in the tree go through the same oracle but are
    # separate corpora: language constructs no real posterior happens to
    # use, and brms output. This doc reports the posteriordb sweep, so
    # drop them before anything is counted or tabulated
    # (tools/gen_docs.py splits them the same way).
    ver = {m: v for m, v in ver.items()
           if not any((d / f"{m}.stan").exists() for d in LOCAL_CORPORA)}
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

    md = ["# Corpus status", "",
          f"Evaluating: {len(ok)}/{len(results)}",
          f"Differentially verified against CmdStan: "
          f"{len(verified)}/{len(results)}", "",
          "A model counts as passing only when tools/verify_sample.py "
          "matches CmdStan's log_prob and full gradient at the shared "
          "deterministic point. Accuracy below is the worst deviation "
          "over lp and every gradient component: relative, and in ULPs "
          "(0 = bitwise identical to CmdStan). Bitwise counts are "
          "reported for information; the numeric policy gate is the ULP "
          "budget. Models that evaluate but are not verified are listed "
          "separately and are not counted.",
          "",
          "| model | values compared | max rel diff | max ULP |",
          "| --- | ---: | ---: | ---: |"]
    for m in verified:
        v = ver[m]
        rel = "0 (bitwise)" if v["max_rel"] == 0 else f"{v['max_rel']:.1e}"
        md.append(f"| `{m}` | {v['n_values']} | {rel} | {v['max_ulp']} |")
    noted = [m for m in verified if m in NOTES]
    if noted:
        md += ["", "Models over the default budget:", ""]
        md += [f"- `{m}`: {NOTES[m]}" for m in noted]
    wa_refs = {}
    if REFS_PATH.exists():
        # Every point carries its own write_array reference; the table
        # counts the primary point's, the one this scoreboard's other
        # columns already describe.
        refs = load_refs()[0]
        for m, v in refs.items():
            pt = v["points"].get(str(v.get("primary")), {})
            if "wa" in pt and m in ver:
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
    unver = [m for m in ok if m not in verified and m not in rejected]
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
    md += rethinking_status()
    (REPO / "docs" / "corpus-status.md").write_text("\n".join(md) + "\n")


def rethinking_status():
    """Keep the teaching corpus separate from posteriordb's denominator."""
    directory = REPO / "tests" / "rethinking"
    models = sorted(p.stem for p in directory.glob("*.stan"))
    refs, _ = load_refs()
    rows = []
    for model in models:
        points = refs.get(model, {}).get("points", {})
        passed = sum(p.get("status") == "VERIFIED" for p in points.values())
        worst = max((p.get("max_rel") or 0 for p in points.values()), default=0)
        rows.append((model, passed, worst))
    verified = sum(passed == 3 for _, passed, _ in rows)
    md = ["", "## Rethinking teaching corpus", "",
          f"Reference recording: {verified}/{len(models)} fixtures verified at all three CmdStan points. "
          "The inventory covers all 61 ulam call sites in chapters 4–16 of the "
          "second edition, plus a supplemental hurdle model. Counts here are "
          "separate from posteriordb. These are the recorder's measurements; "
          "`tools/verify_refs.py` replays them against the current build in CI.",
          "", "See [the inventory and provenance](../tests/rethinking/README.md), and "
          "[current-build replay and performance results](teaching-support.md). "
          "Recording coverage is not a claim that the current build replays every fixture successfully.",
          "", "| model | verified points | worst scaled error |",
          "| --- | ---: | ---: |"]
    md += [f"| `{model}` | {passed}/3 | {worst:.2e} |"
           for model, passed, worst in rows]
    return md


if __name__ == "__main__":
    main()
