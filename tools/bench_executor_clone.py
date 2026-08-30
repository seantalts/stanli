#!/usr/bin/env python3
"""Alternate fresh baseline/candidate processes for Executor layout changes."""

import argparse
import json
import platform
import statistics
import subprocess
from pathlib import Path


METRICS = (
    "graph_ms",
    "bind_ms",
    "clone_ms_total",
    "clone_ms_each",
    "gradient_ns",
    "rss_before_clone_mb",
    "rss_after_clone_mb",
    "peak_rss_mb",
)


def run_one(binary, ops, executors, reps):
    command = [
        str(binary),
        "--ops",
        str(ops),
        "--executors",
        str(executors),
        "--reps",
        str(reps),
    ]
    output = subprocess.check_output(command, text=True).strip()
    fields = dict(token.split("=", 1) for token in output.split())
    for key, expected in (("ops", ops), ("executors", executors), ("reps", reps)):
        if int(fields[key]) != expected:
            raise RuntimeError(f"unexpected {key} from {binary}: {fields[key]}")
    for key in (
        *METRICS,
        "sizeof_op",
        "sizeof_slot",
        "sizeof_ctx",
        "op_bytes_per_executor",
    ):
        fields[key] = float(fields[key])
    return fields


def summary(values):
    quartiles = statistics.quantiles(values, n=4, method="inclusive")
    return {
        "median": statistics.median(values),
        "q1": quartiles[0],
        "q3": quartiles[2],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--ops", type=int, default=25000)
    parser.add_argument("--executors", type=int, nargs="+", default=[1, 8, 32])
    parser.add_argument("--samples", type=int, default=12)
    parser.add_argument("--reps", type=int, default=20)
    parser.add_argument("--json", type=Path, help="retain raw paired samples")
    args = parser.parse_args()
    if args.samples < 4 or args.ops < 1 or args.reps < 1:
        parser.error("samples must be >= 4; ops and reps must be positive")
    if any(count < 1 for count in args.executors):
        parser.error("executor counts must be positive")
    binaries = {
        "baseline": args.baseline.resolve(),
        "candidate": args.candidate.resolve(),
    }
    if binaries["baseline"] == binaries["candidate"]:
        parser.error("baseline and candidate must be distinct binaries")

    report = {
        "platform": platform.platform(),
        "binaries": {key: str(value) for key, value in binaries.items()},
        "ops": args.ops,
        "reps": args.reps,
        "samples": args.samples,
        "results": [],
    }
    for count in args.executors:
        pairs = []
        for sample in range(args.samples):
            # AB, BA, AB, BA produces ABBA/BAAB blocks without reusing a
            # process or its allocator high-water mark across measurements.
            order = ("baseline", "candidate")
            if sample % 2:
                order = tuple(reversed(order))
            pair = {
                name: run_one(binaries[name], args.ops, count, args.reps)
                for name in order
            }
            for key in ("ops_capacity", "slots", "slot_elements", "rss_method"):
                if pair["baseline"][key] != pair["candidate"][key]:
                    raise RuntimeError(f"baseline/candidate mismatch for {key}")
            if pair["baseline"]["sink"] != pair["candidate"]["sink"]:
                raise RuntimeError(f"numerical sink mismatch at {count} executors")
            pairs.append(pair)

        result = {"executors": count, "pairs": pairs, "metrics": {}}
        print(f"executors={count} ops={args.ops} samples={args.samples}")
        for metric in (
            "sizeof_op",
            "sizeof_slot",
            "sizeof_ctx",
            "op_bytes_per_executor",
        ):
            a = pairs[0]["baseline"][metric]
            b = pairs[0]["candidate"][metric]
            print(f"  {metric}: A={a:.0f} B={b:.0f} delta={b - a:+.0f}")
        for metric in METRICS:
            baseline = [pair["baseline"][metric] for pair in pairs]
            candidate = [pair["candidate"][metric] for pair in pairs]
            values = {"baseline": summary(baseline), "candidate": summary(candidate)}
            if all(a > 0 and b >= 0 for a, b in zip(baseline, candidate)):
                values["paired_ratio"] = summary(
                    [b / a for a, b in zip(baseline, candidate)]
                )
            result["metrics"][metric] = values
            a = values["baseline"]
            b = values["candidate"]
            ratio = values.get("paired_ratio")
            ratio_text = (
                f" B/A={ratio['median']:.4f} [{ratio['q1']:.4f}, {ratio['q3']:.4f}]"
                if ratio
                else ""
            )
            print(
                f"  {metric}: A={a['median']:.3f} [{a['q1']:.3f}, {a['q3']:.3f}]"
                f" B={b['median']:.3f} [{b['q1']:.3f}, {b['q3']:.3f}]{ratio_text}"
            )
        report["results"].append(result)

    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
