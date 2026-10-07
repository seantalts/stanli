#!/usr/bin/env python3
"""Speedups of arms over a reference arm from ab_bench_corpus.py results.

python3 harnesses/fd_bench_summary.py RESULTS_DIR --census census.json --ref B [--ref A] \
    [--arms C,D,E,F,G,AA,A] [--json out.json]

A speedup is reference time over arm time (above 1 is faster), paired within
a round, median over rounds per model, geometric mean over models. The 95%
interval resamples models and, within a model, rounds. The same is done for
cycles per gradient when the results carry them.
"""
import argparse
import json
import math
import pathlib
import random
import statistics

SIX = {"normal", "cauchy", "student_t", "lognormal", "beta", "gamma"}


def med_ratio(ref, arm, idx):
    return statistics.median(ref[i] / arm[i] for i in idx)


def gm_ci(models, key, reps=1000, seed=1):
    if not models:
        return float("nan"), float("nan"), float("nan")
    rng = random.Random(seed)
    vals = [math.log(med_ratio(m[key][0], m[key][1], range(len(m[key][0])))) for m in models]
    g = math.exp(statistics.mean(vals))
    boots = []
    for _ in range(reps):
        tot = 0.0
        for _ in models:
            m = models[rng.randrange(len(models))]
            ref, arm = m[key]
            n = len(ref)
            tot += math.log(med_ratio(ref, arm, [rng.randrange(n) for _ in range(n)]))
        boots.append(math.exp(tot / len(models)))
    boots.sort()
    return g, boots[int(0.025 * reps)], boots[int(0.975 * reps)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("results", type=pathlib.Path)
    ap.add_argument("--census", type=pathlib.Path, required=True)
    ap.add_argument("--ref", action="append", required=True)
    ap.add_argument("--arms", default="")
    ap.add_argument("--json", type=pathlib.Path)
    ap.add_argument("--reps", type=int, default=1000)
    args = ap.parse_args()
    census = json.loads(args.census.read_text())["models"]
    recs = [json.loads(line) for line in (args.results / "results.jsonl").open()]
    ok = [r for r in recs if r["status"] == "ok"]
    failed = [r["model"] for r in recs if r["status"] != "ok"]
    arms = list(ok[0]["samples"])
    want = args.arms.split(",") if args.arms else arms
    print(f"{len(ok)} models timed, {len(failed)} failed or excluded: {failed}")
    vec = {m for m, v in census.items() if any(k in SIX and x[1] > 1 for k, x in v.items())}
    scal = {m for m, v in census.items() if SIX & set(v)} - vec
    differs = {}
    out = {}
    for ref in args.ref:
        for arm in want:
            if arm == ref:
                continue
            rows = []
            for r in ok:
                s, c = r["samples"], r.get("cycles", {})
                row = dict(model=r["model"], ns=(s[ref], s[arm]))
                if c.get(ref) and c.get(arm):
                    row["cy"] = (c[ref], c[arm])
                row["same_values"] = r["values"].get(ref) == r["values"].get(arm)
                rows.append(row)
            for m in rows:
                if not m["same_values"]:
                    differs.setdefault(arm, set()).add(m["model"])
            sets = {"all": rows,
                    "vector": [m for m in rows if m["model"] in vec],
                    "scalar": [m for m in rows if m["model"] in scal]}
            line = {}
            for name, sub in sets.items():
                g, lo, hi = gm_ci([dict(ns=m["ns"]) for m in sub], "ns", args.reps)
                cy = [dict(cy=m["cy"]) for m in sub if "cy" in m]
                cg = gm_ci(cy, "cy", args.reps)[0] if cy else float("nan")
                line[name] = dict(n=len(sub), geomean=g, lo=lo, hi=hi, cycles_geomean=cg)
            per = sorted(((med_ratio(*m["ns"], range(len(m["ns"][0]))), m["model"],
                           med_ratio(*m["cy"], range(len(m["cy"][0]))) if "cy" in m else float("nan"))
                          for m in rows))
            n_fast = sum(1 for p in per if p[0] > 1.02)
            n_slow = sum(1 for p in per if p[0] < 0.98)
            print(f"\n{arm} over {ref}: " + "; ".join(
                f"{k} n={v['n']} {v['geomean']:.4f} ({v['lo']:.4f} to {v['hi']:.4f}) cycles {v['cycles_geomean']:.4f}"
                for k, v in line.items()))
            print(f"  faster than 1.02: {n_fast}, within 2%: {len(per) - n_fast - n_slow}, "
                  f"slower than 0.98: {n_slow}, below 0.90: {sum(1 for p in per if p[0] < 0.90)}")
            print("  best : " + ", ".join(f"{p[1]} {p[0]:.3f} (cy {p[2]:.3f})" for p in per[::-1][:6]))
            print("  worst: " + ", ".join(f"{p[1]} {p[0]:.3f} (cy {p[2]:.3f})" for p in per[:6]))
            out[f"{arm}/{ref}"] = dict(line=line, per_model={p[1]: [p[0], p[2]] for p in per})
    for arm, ms in differs.items():
        print(f"values differ from a reference for {arm}: {sorted(ms)}")
    if args.json:
        args.json.write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
