#!/usr/bin/env python3
"""Validation helpers for archived paired sampling reports.

These retain the historical report's speed thresholds and posterior comparison
protocol. Current benchmarks and corpus smoke tests do not enforce those
collection-specific thresholds. Reproduce measurements using the revision and
commands recorded with each archived report.
"""
import json
import math
import pathlib
import statistics

REPO = pathlib.Path(__file__).resolve().parents[1]
MINIMUM_SPEEDUPS = {"aalto_gpareto": 1.0}


def inventory():
    """Model membership of the archived Aalto paired-sampling reports."""
    # This experiment predates later corpus additions. Its membership must not
    # change when the live inventory imports another model.
    report = REPO / "tests/educational/pareto-benchmark-results.json"
    return sorted(json.loads(report.read_text())["models"])


def speed_gate(measurements, minimum=0.5):
    if set(measurements) != {"stanli", "cmdstan"}:
        raise ValueError("Both engines must be measured")
    if len(measurements["stanli"]) != len(measurements["cmdstan"]):
        raise ValueError("Timing repetitions must be paired")
    for values in measurements.values():
        if len(values) < 3 or not all(math.isfinite(x) and x > 0 for x in values):
            raise ValueError("Need >=3 finite, positive timings per engine")
    medians = {engine: statistics.median(values) for engine, values in measurements.items()}
    dispersion = {engine: {"min": min(values), "max": max(values),
                           "mad": statistics.median(abs(x-medians[engine]) for x in values)}
                  for engine, values in measurements.items()}
    speedup = medians["cmdstan"] / medians["stanli"]
    return {"seconds": measurements, "medians": medians, "dispersion": dispersion,
            "speedup": speedup, "minimum_speedup": minimum, "pass": speedup >= minimum}


def posterior_gate(chains, names):
    """Compare parameter means using batch-means Monte Carlo uncertainty.

    Twenty contiguous batches per chain retain within-chain correlation;
    the between-chain estimate is a lower bound on the uncertainty too.
    Six combined standard errors is a broad regression test, not evidence
    of convergence or an assertion that two random trajectories must match.
    """
    result = {}
    for name in names:
        summaries = {}
        for engine, runs in chains.items():
            batches, means = [], []
            for rows in runs:
                values = [r[name] for r in rows]
                size = len(values) // 20
                if size < 2:
                    raise ValueError("Posterior comparison needs at least 40 samples")
                batches.extend(statistics.mean(values[i*size:(i+1)*size]) for i in range(20))
                means.append(statistics.mean(values))
            se = max(math.sqrt(statistics.variance(batches) / len(batches)),
                     math.sqrt(statistics.variance(means) / len(means)))
            summaries[engine] = {"mean": statistics.mean(means), "mcse": se}
        a, b = summaries["stanli"], summaries["cmdstan"]
        limit = 6 * math.hypot(a["mcse"], b["mcse"]) + 1e-9 * max(1, abs(a["mean"]), abs(b["mean"]))
        result[name] = {**summaries, "difference": abs(a["mean"]-b["mean"]),
                        "limit": limit, "pass": abs(a["mean"]-b["mean"]) <= limit}
    if not result:
        raise ValueError("No parameter columns for posterior comparison")
    return result
