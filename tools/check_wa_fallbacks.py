#!/usr/bin/env python3
"""Fail when write_array falls back to the MIR interpreter unexpectedly.

Runs stanli_check on the container write/read grid and the OncoBayes2 corpus
models and reads the `(interpreted: BLOCK (REASON))` note it prints. A model
that falls back must be listed in tests/wa_fallback_ledger.json with that exact
note; a listed model that no longer falls back, or falls back for a different
reason, also fails, so the ledger can only shrink.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
GRID = REPO / "tests/function_coverage/container_flow"
EMPTY = REPO / "tests/function_coverage/empty.json"
LEDGER = REPO / "tests/wa_fallback_ledger.json"


def models():
    for stan in sorted(GRID.glob("*.stan")):
        data = stan.with_suffix(".json")
        yield stan, data if data.exists() else EMPTY
    for stan in sorted((REPO / "tests/oncobayes2").glob("*.stan")):
        yield stan, stan.with_suffix(".json")


def fallback(stan, data, args):
    argv = [args.build / "stanli_check", stan, data]
    if args.stanc:
        argv += ["--stanc", args.stanc]
    proc = subprocess.run([str(x) for x in argv], cwd=REPO, text=True,
                          capture_output=True, timeout=600)
    wa = [line for line in proc.stderr.splitlines() if line.startswith("WA ")]
    if proc.returncode or not wa:
        raise RuntimeError(f"{stan.name}: stanli_check failed ({proc.returncode})\n"
                           + proc.stdout[-2000:] + proc.stderr[-2000:])
    match = re.search(r"\(interpreted: (.*)\)$", wa[-1])
    return stan.stem, match[1] if match else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build-rel")
    parser.add_argument("--stanc", type=pathlib.Path)
    parser.add_argument("--ledger", type=pathlib.Path, default=LEDGER)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    args.build = args.build.resolve()
    ledger = {e["model"]: e for e in json.loads(args.ledger.read_text())}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        found = dict(pool.map(lambda m: fallback(*m, args), models()))
    failures = []
    for name in sorted(set(found) | set(ledger)):
        got, entry = found.get(name), ledger.get(name)
        want = entry["interpreted"] if entry else None
        if name not in found:
            failures.append(f"{name}: in the ledger but not a checked model")
        elif got == want:
            continue
        elif want is None:
            failures.append(f"{name}: write_array now runs interpreted: {got}")
        elif got is None:
            failures.append(f"{name}: no longer falls back; delete its ledger entry")
        else:
            failures.append(f"{name}: ledger says '{want}', now '{got}'")
    for failure in failures:
        print("FAIL", failure)
    print(f"{len(found)} models, {sum(v is not None for v in found.values())} "
          f"interpreted, {len(ledger)} ledger entries, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
