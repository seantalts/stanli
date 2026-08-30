#!/usr/bin/env python3
"""Publish compact, input-free evidence from a runtime_ab results.json file.

The original artifact directory remains authoritative. This export retains
every sample, numerical comparison and preparation profile without embedding
model data, MIR, per-value gradients or machine-specific subprocess paths.
"""

import argparse
import csv
import json
from pathlib import Path

from runtime_ab import digest, write_report


def portable_report(report):
    metadata = {k: v for k, v in report["metadata"].items()
                if k not in ("command", "driver_commands")}
    results = []
    for source in report["results"]:
        row = {k: source[k] for k in
               ("name", "group", "status", "benchmark_point", "evaluations", "params", "summary")
               if k in source}
        row["inputs"] = source.get("inputs", {}).get("sha256", {})
        row["correctness"] = []
        for check in source["correctness"]:
            portable = {k: v for k, v in check.items() if k != "calls"}
            portable["calls"] = {
                side: {k: call[k] for k in ("status", "returncode", "environment")}
                for side, call in check["calls"].items()}
            row["correctness"].append(portable)
        row["samples"] = []
        for pair in source["samples"]:
            sample = {"order": pair["order"]}
            for side in ("before", "after"):
                sample[side] = {
                    phase: {k: call[k] for k in
                            ("status", "returncode", "environment", "measurement", "peak_rss_mib")}
                    for phase, call in pair[side].items()}
            row["samples"].append(sample)
        row["profiles"] = {
            side: {k: profile[k] for k in ("status", "returncode", "environment", "rows")}
            for side, profile in source["profiles"].items()}
        results.append(row)
    return {"metadata": metadata, "results": results}


def write_samples(report, path):
    fields = ["model", "group", "sample", "order", "side", "point", "evaluations",
              "gradient_ns", "forward_ns", "prep_s", "gradient_peak_rss_mib",
              "prep_peak_rss_mib"]
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fields, lineterminator="\n")
        writer.writeheader()
        for row in report["results"]:
            for index, pair in enumerate(row["samples"]):
                for side in pair["order"]:
                    prep, grad = (pair[side][p] for p in ("prep", "gradient"))
                    pm, gm = prep.get("measurement") or {}, grad.get("measurement") or {}
                    writer.writerow(dict(
                        model=row["name"], group=row["group"], sample=index,
                        order="/".join(pair["order"]), side=side,
                        point=row["benchmark_point"], evaluations=row["evaluations"],
                        gradient_ns=gm.get("gradient_ns"), forward_ns=gm.get("forward_ns"),
                        prep_s=pm.get("prep_s"), gradient_peak_rss_mib=grad["peak_rss_mib"],
                        prep_peak_rss_mib=prep["peak_rss_mib"]))


def export_report(source, output):
    report = portable_report(json.loads(source.read_text()))
    report["metadata"]["original_results_sha256"] = digest(source)
    write_report(report, output)
    # Machine-readable evidence: one model per line avoids tens of thousands
    # of indentation-only lines in a review while preserving every value.
    records = [json.dumps(row, separators=(",", ":"), allow_nan=False)
               for row in report["results"]]
    (output / "results.json").write_text(
        '{"metadata":' + json.dumps(report["metadata"], separators=(",", ":")) +
        ',"results":[\n' + ",\n".join(records) + "\n]}\n")
    write_samples(report, output / "samples.csv")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    export_report(args.results, args.output)


if __name__ == "__main__":
    main()
