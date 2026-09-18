#!/usr/bin/env python3
"""Run complete source-to-CSV sampling smoke tests from the corpus inventory.

By default only cases with sampling_smoke=true in their metadata are sampled.
--collection filters that opt-in set; --models explicitly selects any corpus
cases. Numerical replay uses tools/verify_refs.py; this smoke test checks 100
seeded draws for complete, correctly ordered, finite output and diagnostics.
It is not a benchmark or a posterior-convergence test.
"""
import argparse
import csv
import io
import json
import math
import pathlib
import re
import subprocess
import sys
import tempfile

from check_function_models import source_digest
from corpus_inventory import corpus_cases, materialize_data
from verify_refs import REFS_PATH, default_check_bin, load_refs

REPO = pathlib.Path(__file__).resolve().parents[1]
SAMPLER_COLUMNS = ["lp__", "accept_stat__", "stepsize__", "treedepth__",
                   "n_leapfrog__", "divergent__", "energy__"]


def selected_cases(cases, models=None):
    if models is not None:
        if len(set(models)) != len(models):
            raise ValueError("Duplicate model selectors")
        missing = set(models) - set(cases)
        if missing:
            raise ValueError("Unknown models in selected collection: " + ", ".join(sorted(missing)))
        selected = {name: cases[name] for name in models}
    else:
        selected = {name: case for name, case in cases.items()
                    if case.metadata.get("sampling_smoke") is True}
    if not selected:
        raise ValueError("No sampling smoke cases selected")
    return dict(sorted(selected.items()))


def reference_names(reference, source, data):
    for key, path in (("source_sha256", source), ("data_sha256", data)):
        if key in reference and reference[key] != source_digest(path):
            raise ValueError("Reference source/data hashes differ")
    point = reference.get("points", {}).get(str(reference.get("primary", 0)), {})
    names = point.get("wa", {}).get("names")
    if not isinstance(names, str) or not names:
        raise ValueError("Missing CmdStan output names at the primary reference point")
    result = names.split(",")
    if any(not name for name in result) or len(set(result)) != len(result):
        raise ValueError("Invalid CmdStan output names")
    return result


def sample_csv(text, names, samples):
    rows = list(csv.reader(line for line in io.StringIO(text)
                           if line.strip() and not line.startswith("#")))
    if not rows or len(rows) != samples + 1:
        raise ValueError(f"Expected {samples} draws, got {max(0, len(rows)-1)}")
    header = rows[0]
    if header[:len(SAMPLER_COLUMNS)] != SAMPLER_COLUMNS:
        raise ValueError("Sample CSV must contain all seven standard sampler diagnostics in order")
    outputs = header[len(SAMPLER_COLUMNS):]
    if outputs != names or len(set(header)) != len(header):
        raise ValueError("Sample CSV output names/order differ from CmdStan")
    result = []
    for row in rows[1:]:
        if len(row) != len(header):
            raise ValueError("Ragged sample CSV")
        values = [float(x) for x in row]
        if not all(math.isfinite(x) for x in values):
            raise ValueError("Nonfinite sample, diagnostic, or generated quantity")
        result.append(dict(zip(header, values)))
    return result


def parse_timings(stderr):
    match = re.search(r"stanli_run: timings prep_s=(\S+) sample_s=(\S+) output_s=(\S+)", stderr)
    if not match:
        raise ValueError("Missing phase timing output")
    result = dict(zip(("prep_s", "sample_s", "output_s"), map(float, match.groups())))
    if not all(math.isfinite(x) and x >= 0 for x in result.values()):
        raise ValueError("Invalid phase timing")
    return result

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=default_check_bin().parent)
    parser.add_argument("--pdb", type=pathlib.Path, default=REPO / "deps/posteriordb")
    parser.add_argument("--refs", type=pathlib.Path, default=REFS_PATH)
    parser.add_argument("--collection", default="all")
    parser.add_argument("--models", nargs="+")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    try:
        cases = selected_cases(corpus_cases(args.pdb, collection=args.collection), args.models)
        refs, _ = load_refs(args.refs)
        missing = set(cases) - set(refs)
        if missing:
            raise ValueError("Missing CmdStan references: " + ", ".join(sorted(missing)))
    except ValueError as error:
        parser.error(str(error))
    binary = args.build.resolve() / ("stanli_run.exe" if sys.platform == "win32" else "stanli_run")
    report = {"models": {}, "settings": {"warmup": 100, "samples": 100, "seed": 42,
              "collection": args.collection, "models": args.models}}
    failures = []
    with tempfile.TemporaryDirectory(prefix="stanli_sampling_") as temporary:
        for name in cases:
            try:
                source = cases[name].source
                data = materialize_data(cases[name], pathlib.Path(temporary))
                names = reference_names(refs[name], source, data)
                process = subprocess.run([str(binary), str(source), str(data), "--seed", "42",
                                          "--warmup", "100", "--samples", "100", "--init-radius", "0",
                                          "--sampler-stats"], cwd=REPO, text=True, capture_output=True,
                                         timeout=args.timeout, check=True)
                sample_csv(process.stdout, names, 100)
                report["models"][name] = {"draws": 100, "output_columns": len(names), "pass": True}
                print(f"PASS {name}: 100 draws, {len(names)} output columns", flush=True)
            except (ValueError, OSError, subprocess.SubprocessError) as error:
                message = str(error)
                if isinstance(error, subprocess.CalledProcessError):
                    message += ": " + (error.stderr or "")[-2000:]
                failures.append(name)
                report["models"][name] = {"pass": False, "error": message}
                print(f"FAIL {name}: {message}", flush=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(f"{len(cases)-len(failures)}/{len(cases)} corpus sampling smoke cases passed", flush=True)
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
