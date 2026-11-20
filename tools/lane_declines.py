#!/usr/bin/env python3
"""Rank why reroll and partition leave matching lanes unfused, over the corpus.

python3 tools/lane_declines.py PDB OUT_DIR --build BUILD [--jobs 6] [--models a,b]

Compiles every corpus model with BUILD's embedded compiler, prepares it with
BUILD/dump_ops under STANLI_LANE_REPORT=1, and counts the LANES lines that the
two passes print (runtime/include/stanli/lane_decline.hpp). Reroll tries
several periods at one position, so a position counts once, under the reason
of the shortest period it tried. OUT_DIR/lane_declines.json keeps every
record, and the table goes to stdout as Markdown.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from corpus_inventory import corpus_cases, materialize_data

ENV = {"STANLI_LANE_REPORT": "1", "STAN_NUM_THREADS": "1", "OMP_NUM_THREADS": "1"}


def probe_path(build):
    cache = (build / "CMakeCache.txt").read_text()
    source = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache, re.M).group(1)
    embed = subprocess.check_output(
        ["bash", "-c", "source tools/stanc_embed/provenance.sh && stanc_embed_dir"],
        cwd=source, text=True).strip()
    return pathlib.Path(embed) / "stanli-vectorize-probe"


def run_model(case, data, build, probe, out):
    work = out / case.name
    work.mkdir(parents=True, exist_ok=True)
    mir = work / "model.sexp"
    done = subprocess.run([str(probe), "--vectorize-loops", "on", "--output", str(mir),
                           str(case.source)], capture_output=True, text=True, timeout=900)
    if done.returncode:
        return case.name, None, "compile failed"
    done = subprocess.run([str(build / "dump_ops"), str(mir), str(data), "-1", "summary"],
                          capture_output=True, text=True, timeout=600,
                          env={**os.environ, **ENV})
    if done.returncode:
        return case.name, None, f"dump_ops exit {done.returncode}"
    records = [json.loads(line[len("LANES "):]) for line in done.stderr.splitlines()
               if line.startswith("LANES {")]
    return case.name, records, None


def first_per_position(records):
    """One record per (graph, pass, op): the one with the shortest period."""
    best = {}
    for r in records:
        if r["pass"] == "terms":
            continue
        key = (r["graph"], r["pass"], r["op"])
        if key not in best or r["period"] < best[key]["period"]:
            best[key] = r
    return list(best.values())


def table(results, collection_of):
    counts = Counter()
    models = defaultdict(set)
    for name, records in results.items():
        for r in first_per_position(records):
            key = (r["pass"], r["graph"], r["reason"])
            counts[key] += 1
            models[key].add(name)
    lines = ["| pass | graph | reason | positions | models | collections |",
             "|---|---|---|---:|---:|---:|"]
    for key, n in sorted(counts.items(), key=lambda kv: (-len(models[kv[0]]), -kv[1])):
        cols = {collection_of[m] for m in models[key]}
        lines.append(f"| {key[0]} | {key[1]} | {key[2]} | {n} | {len(models[key])} "
                     f"| {len(cols)} |")
    repeated = sorted(n for n, rs in results.items()
                      if any(r["pass"] == "terms" and r["repeated"] > 0 for r in rs))
    none = sorted(n for n, rs in results.items() if not first_per_position(rs))
    lines.append("")
    lines.append(f"Models with no declined group: {len(none)} of {len(results)}.")
    lines.append(f"Models whose target lists one slot more than once: {len(repeated)}"
                 + (f" ({', '.join(repeated)})" if repeated else "") + ".")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pdb", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--models", default="", help="comma-separated subset")
    args = parser.parse_args(argv)
    build = args.build.absolute()
    probe = probe_path(build)
    out = args.out.absolute()
    out.mkdir(parents=True, exist_ok=True)
    cases = corpus_cases(args.pdb, "all")
    if args.models:
        cases = {n: cases[n] for n in args.models.split(",")}
    results, failures = {}, {}
    with tempfile.TemporaryDirectory() as unzipped:
        data = {n: materialize_data(c, unzipped) for n, c in cases.items()}
        with ThreadPoolExecutor(args.jobs) as pool:
            jobs = [pool.submit(run_model, c, data[n], build, probe, out)
                    for n, c in sorted(cases.items())]
            for job in jobs:
                name, records, failure = job.result()
                if failure:
                    failures[name] = failure
                else:
                    results[name] = records
    (out / "lane_declines.json").write_text(
        json.dumps(dict(results=results, failures=failures), indent=1, sort_keys=True) + "\n")
    print(table(results, {n: c.collection for n, c in cases.items()}))
    if failures:
        print("\nFailed: " + ", ".join(f"{n} ({f})" for n, f in sorted(failures.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
