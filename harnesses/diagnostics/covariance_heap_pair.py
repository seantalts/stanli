#!/usr/bin/env python3
"""Run alternating fresh-process covariance heap pairs, retaining every result."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys


CASES = [("udf", 1), ("udf", 128), ("udf", 256),
         ("builtin_equivalent", 1), ("builtin_equivalent", 128),
         ("builtin_equivalent", 256), ("normal_canary", 100)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=6)
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    driver = Path(__file__).with_name("covariance_heap_probe.py")
    profiles, checks, failures = [], [], []
    for variant, size in CASES:
        reference = None
        for trial in range(args.pairs):
            pair = {}
            order = ["base", "candidate"] if trial % 2 == 0 else ["candidate", "base"]
            for label in order:
                runtime = getattr(args, label).resolve()
                path = args.output / label / (f"{variant}-{size}-trial{trial}.json")
                command = [sys.executable, str(driver), "--runtime", str(runtime),
                           "--variant", variant, "--size", str(size), "--output", str(path)]
                completed = subprocess.run(command, text=True, capture_output=True)
                if completed.returncode:
                    failures.append(dict(variant=variant, N=size, trial=trial,
                                         label=label, command=command,
                                         returncode=completed.returncode,
                                         stdout=completed.stdout, stderr=completed.stderr))
                    continue
                profile = json.loads(path.read_text())
                profile.update(label=label, trial=trial, order=order)
                profiles.append(profile)
                pair[label] = profile
            if len(pair) != 2:
                continue
            base, candidate = pair["base"], pair["candidate"]
            equal = {key: base[key] == candidate[key] for key in
                     ["source_sha256", "mir_sha256", "data", "n_unconstrained",
                      "output_columns", "harness_sha256"]}
            hashes = {}
            for left, right in zip(base["observations"], candidate["observations"]):
                stage = left["stage"]
                equal[stage + "_stage"] = stage == right["stage"]
                for key in ["lp_gradient_sha256", "q_sha256", "output_sha256"]:
                    if key in left:
                        equal[stage + "_" + key] = left[key] == right[key]
                        hashes[stage + "_" + key] = left[key]
            equal["same_stage_count"] = len(base["observations"]) == len(candidate["observations"])
            if reference is None:
                reference = hashes
            equal["deterministic_across_trials"] = reference == hashes
            checks.append(dict(variant=variant, N=size, trial=trial, checks=equal))
            print(variant, size, "trial", trial, "exact", all(equal.values()), flush=True)
    summary = []
    metrics = ["elapsed_seconds", "size_in_use", "size_allocated", "current_rss_bytes", "peak_rss_bytes"]
    for variant, size in CASES:
        for label in ["base", "candidate"]:
            selected = [p for p in profiles if p["variant"] == variant and p["N"] == size and p["label"] == label]
            stages = sorted({r["stage"] for p in selected for r in p["observations"]})
            for stage in stages:
                row = dict(variant=variant, N=size, label=label, stage=stage)
                for metric in metrics:
                    values = [r[metric] for p in selected for r in p["observations"] if r["stage"] == stage]
                    row[metric] = dict(n=len(values), median=statistics.median(values),
                                       minimum=min(values), maximum=max(values),
                                       mean=statistics.mean(values),
                                       stdev=statistics.stdev(values) if len(values) > 1 else 0.0)
                summary.append(row)
    result = dict(requested_pairs=args.pairs, cases=CASES, failures=failures,
                  checks=checks, profiles=profiles, summary=summary)
    (args.output / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if failures or len(checks) != len(CASES) * args.pairs or not all(all(row["checks"].values()) for row in checks):
        raise SystemExit("Incomplete or non-identical pair: inspect comparison.json")


if __name__ == "__main__":
    main()
