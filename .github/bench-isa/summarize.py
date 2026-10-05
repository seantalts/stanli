#!/usr/bin/env python3
import csv
import statistics
import sys
from collections import defaultdict


def quant(xs, q):
    xs = sorted(xs)
    k = (len(xs) - 1) * q
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def main(path):
    rows = [r for r in csv.DictReader(open(path), delimiter="\t")
            if r["status"] == "ok"]
    print(f"{len(rows)} models timed")
    speedups = [k for k in rows[0] if k.endswith("_speedup")]
    for k in speedups:
        xs = [float(r[k]) for r in rows]
        geo = statistics.geometric_mean(xs)
        print(f"\n{k}: median {statistics.median(xs):.3f}x  "
              f"q1 {quant(xs, .25):.3f}  q3 {quant(xs, .75):.3f}  "
              f"geomean {geo:.3f}  min {min(xs):.3f}  max {max(xs):.3f}")
        ranked = sorted(rows, key=lambda r: float(r[k]))
        print("  worst 10: " + ", ".join(
            f"{r['model']} {float(r[k]):.2f}" for r in ranked[:10]))
        print("  best 10:  " + ", ".join(
            f"{r['model']} {float(r[k]):.2f}" for r in ranked[::-1][:10]))
        by = defaultdict(list)
        for r in rows:
            by[r["collection"]].append(float(r[k]))
        for c, v in sorted(by.items()):
            print(f"  {c:12s} n={len(v):3d} median {statistics.median(v):.3f}")


if __name__ == "__main__":
    main(sys.argv[1])
