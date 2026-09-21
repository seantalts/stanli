#!/usr/bin/env python3
"""Print the three name-level coverage rows in docs/coverage.md.

Reads docs/conformance-baseline.json.gz and counts, per family, the names
with no unexpected_unsupported signature.

  python3 tools/coverage_summary.py
"""
import collections
import gzip
import json
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parent.parent
BASELINE = REPO / "docs" / "conformance-baseline.json.gz"


def main():
    with gzip.open(BASELINE, "rt") as handle:
        rows = json.load(handle)["classifications"]

    by_name = collections.defaultdict(list)
    for signature, result in rows.items():
        name = signature.split("(", 1)[0]
        by_name[name].append((signature, result["status"]))

    def tally(names):
        names = sorted(names)
        covered = sum(
            all(status != "unexpected_unsupported" for _, status in by_name[name])
            for name in names
        )
        return covered, len(names)

    densities = {name for name in by_name if name.endswith(("_lpdf", "_lpmf"))}
    cdfs = {name for name in by_name if name.endswith(("_cdf", "_lcdf", "_lccdf"))}

    excluded = (
        "_lpdf", "_lpmf", "_rng", "_cdf", "_lcdf", "_lccdf", "_qf", "_log_qf"
    )
    scalar_math = set()
    pattern = re.compile(r"^([A-Za-z_][A-Za-z_0-9]*)\((.*?)\)=>(.*)$")
    for signature in rows:
        match = pattern.match(signature)
        if not match:
            continue
        name, arguments, result = match.groups()
        argument_types = arguments.split(",") if arguments else []
        if (
            result == "real"
            and 1 <= len(argument_types) <= 5
            and all(argument == "real" for argument in argument_types)
            and not name.endswith(excluded)
        ):
            scalar_math.add(name)

    print("densities", tally(densities))
    print("distribution functions", tally(cdfs))
    print("scalar math", tally(scalar_math))


if __name__ == "__main__":
    main()
