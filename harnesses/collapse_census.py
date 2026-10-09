#!/usr/bin/env python3
"""Which likelihood terms does the fast-mode observation collapse reach?

Compiles every corpus model in fast mode with STANLI_COLLAPSE_REPORT=1, so
the pass in runtime/src/collapse.cpp reports each likelihood term it looked
at: observations, rows that differ by value, groups for the densities with a
sufficient statistic, the form it chose (rows, groups or linear), or the
reason the term was left alone.

Usage: python3 harnesses/collapse_census.py deps/posteriordb
           [--check BIN] [--jobs N] [--out FILE.json] [model ...]
Needs build-rel/stanli_check built with a compiler that accepts --fast-math.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import verify_refs  # noqa: E402


def census_model(model, ref, pdb, check_bin, tmp, timeout):
    """(model, status, [term or opaque rows])."""
    stan, data = verify_refs.model_files(model, ref, pdb, tmp)
    env = dict(os.environ, STANLI_COLLAPSE_REPORT="1")
    try:
        proc = subprocess.run(
            [str(check_bin), str(stan), str(data), "--point", "0",
             "--fast-math"],
            capture_output=True, text=True, cwd=REPO, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return (model, "TIMEOUT", [])
    got = verify_refs.parse_status(proc.stdout)
    rows = [json.loads(line[len("COLLAPSE "):])
            for line in proc.stderr.splitlines()
            if line.startswith("COLLAPSE {")]
    return (model, got[0] if got else "CRASH", rows)


def best(term):
    """Elements left after the collapse the term would get."""
    return term["groups"] if term["groups"] >= 0 else term["rows"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("models", nargs="*")
    ap.add_argument("--check", type=pathlib.Path,
                    default=REPO / "build-rel" / "stanli_check")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--out", type=pathlib.Path)
    args = ap.parse_args()

    pdb = args.pdb / "posterior_database"
    refs, _ = verify_refs.replay_refs(verify_refs.native_platform())
    models = args.models or sorted(refs)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_collapse_"))
    results = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(census_model, m, refs[m], pdb, args.check, tmp,
                            args.timeout) for m in models]
        for fut in concurrent.futures.as_completed(futs):
            model, status, rows = fut.result()
            results[model] = {"status": status, "rows": rows}
    if args.out:
        args.out.write_text(json.dumps(results, indent=1, sort_keys=True))

    statuses = collections.Counter(r["status"] for r in results.values())
    refusals, opaque = collections.Counter(), collections.Counter()
    collapsing = []
    for model, r in sorted(results.items()):
        terms = [t for t in r["rows"] if "opcode" in t]
        for o in (o for o in r["rows"] if "opaque" in o):
            opaque[o["opaque"]] += 1
        for t in terms:
            if t["refusal"]:
                refusals[t["refusal"]] += 1
        kept = [t for t in terms if not t["refusal"]]
        if kept:
            top = max(kept, key=lambda t: t["n"] / max(best(t), 1))
            collapsing.append((top["n"] / max(best(top), 1), model, top,
                               len(kept)))
    print(f"{len(results)} models: "
          + ", ".join(f"{n} {s}" for s, n in sorted(statuses.items())))
    print(f"{len(collapsing)} models have a term that collapses; "
          f"{sum(c[0] >= 10 for c in collapsing)} by 10x or more in that term")
    print("\nmodel\tterms\topcode\tform\tops\tn\trows\tgroups\tfactor")
    for factor, model, t, kept in sorted(collapsing, reverse=True):
        print(f"{model}\t{kept}\t{t['opcode']}\t{t.get('evaluator', '')}\t"
              f"{t['ops']}\t{t['n']}\t{t['rows']}\t{t['groups']}\t"
              f"{factor:.1f}")
    print("\nterms left alone, by reason:")
    for why, n in refusals.most_common():
        print(f"  {n}\t{why}")
    print("\nmodels holding an op the analysis cannot see into, by opcode:")
    for name, n in opaque.most_common():
        print(f"  {n}\t{name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
