#!/usr/bin/env python3
"""Interleaved fresh-process A/B timing of bench_grad builds.

  ab_bench.py --arm base=BIN --arm new=BIN2,VAR=1 --arm aa=BIN_COPY \
      --models DIR [DIR ...] --pairs 12

The first arm is the baseline. Each round runs every arm once in a
rotated order, one process at a time. Per arm it reports the median ns per
gradient and MAD, and for every other arm the median and MAD of the
within-round baseline/arm ratio, so a value above 1 is faster than the
baseline.
"""
import argparse
import json
import os
import statistics
import subprocess


def once(binary, model_dir, warmup, measure, env):
    out = subprocess.run(
        [binary, os.path.join(model_dir, "model.tmir.sexp"),
         os.path.join(model_dir, "data.json"), "--timed",
         "--warmup-ms", str(warmup), "--measure-ms", str(measure)],
        capture_output=True, text=True, check=True, env=env).stdout
    j = json.loads(out)
    return j["elapsed_ns"] / j["iterations"]


def mad(xs):
    m = statistics.median(xs)
    return statistics.median([abs(x - m) for x in xs])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--models", nargs="+", required=True)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--warmup-ms", type=int, default=200)
    ap.add_argument("--measure-ms", type=int, default=500)
    ap.add_argument("--json")
    a = ap.parse_args()
    arms = [x.split("=", 1) for x in a.arm]
    envs = {}
    for i, (n, b) in enumerate(arms):
        parts = b.split(",")
        arms[i] = [n, parts[0]]
        envs[n] = dict(kv.split("=", 1) for kv in parts[1:])
    names = [n for n, _ in arms]
    result = {}
    for m in a.models:
        mname = os.path.basename(m.rstrip("/"))
        times = {n: [] for n in names}
        for r in range(a.pairs):
            order = names[r % len(names):] + names[:r % len(names)]
            if r % 2:
                order = order[::-1]
            for n in order:
                b = dict(arms)[n]
                times[n].append(once(b, m, a.warmup_ms, a.measure_ms,
                                     dict(os.environ, **envs[n])))
        row = {}
        base = names[0]
        for n in names:
            row[n] = {"median_ns": statistics.median(times[n]),
                      "mad_ns": mad(times[n]), "samples": times[n]}
            if n != base:
                ratios = [b0 / x for b0, x in zip(times[base], times[n])]
                row[n]["ratio_median"] = statistics.median(ratios)
                row[n]["ratio_mad"] = mad(ratios)
        result[mname] = row
        line = f"{mname:28s}"
        for n in names:
            line += f" {n}: {row[n]['median_ns']:8.1f}+-{row[n]['mad_ns']:5.1f}"
            if n != base:
                line += (f" ({row[n]['ratio_median']:.3f}x"
                         f"+-{row[n]['ratio_mad']:.3f})")
        print(line, flush=True)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result, f, indent=1)


if __name__ == "__main__":
    main()
