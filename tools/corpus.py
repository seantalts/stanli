#!/usr/bin/env python3
"""Corpus coverage harness: run stanli_check over every posteriordb
(model, dataset) pair and histogram the failures by missing feature.

Usage: tools/corpus.py PDB_DIR [--filter SUBSTR]
PDB_DIR is a posteriordb checkout containing posterior_database/.
Writes docs/corpus-status.md and prints the failure histogram.
"""
import collections
import gzip
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from verify_refs import default_check_bin  # noqa: E402

CHECK = default_check_bin()

# Verification results written by tools/verify_sample.py. A model counts as
# passing only if it appears here as VERIFIED; compiling and returning a
# finite gradient is never sufficient.
VERIFY_JSON = REPO / "docs" / "verification.json"


# Context for models that evaluate but do not match, so the reason is not
# lost between runs.
NOTES = {
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
                    text=True, timeout=120, cwd=REPO).stdout.strip()
                if out.startswith("OK"):
                    break
        except subprocess.TimeoutExpired:
            out = "EVAL_FAIL timeout"
        if out.startswith("OK"):
            results[model] = ("OK", "")
        else:
            first = out.split("\n")[0]
            status, _, msg = first.partition(" ")
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
    # tests/stanc3 models go through the same oracle but are a separate
    # corpus: language constructs no real posterior happens to use. This
    # doc reports the posteriordb sweep, so drop them before anything is
    # counted or tabulated (tools/gen_docs.py splits them the same way).
    ver = {m: v for m, v in ver.items()
           if not (REPO / "tests" / "stanc3" / f"{m}.stan").exists()}
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
          "(0 = bitwise identical to CmdStan). Models that evaluate but "
          "are not verified are listed separately and are not counted.",
          "",
          "| model | values compared | max rel diff | max ULP |",
          "| --- | ---: | ---: | ---: |"]
    for m in verified:
        v = ver[m]
        rel = "0 (bitwise)" if v["max_rel"] == 0 else f"{v['max_rel']:.1e}"
        md.append(f"| `{m}` | {v['n_values']} | {rel} | {v['max_ulp']} |")
    refs_path = REPO / "docs" / "corpus-refs.json.gz"
    wa_refs = {}
    if refs_path.exists():
        refs = json.loads(gzip.decompress(refs_path.read_bytes()))
        wa_refs = {m: len(v["wa"]["values"])
                   for m, v in refs.items() if "wa" in v and m in ver}
    if wa_refs:
        md += ["", "## write_array references", "",
               "For models whose generated quantities are deterministic "
               "(no `_rng`), the oracle also records CmdStan's write_array "
               "at the same point: every CSV column (constrained "
               "parameters, transformed parameters, generated quantities). "
               "tools/verify_refs.py replays them in CI with the column "
               "names matched exactly and the values sharing the model's "
               "gate. Models with RNG draws are exercised structurally "
               "(all columns produced and finite) by "
               "harnesses/wa_coverage.py instead, since their values are "
               "a property of the RNG stream.", "",
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
    (REPO / "docs" / "corpus-status.md").write_text("\n".join(md) + "\n")


if __name__ == "__main__":
    main()
