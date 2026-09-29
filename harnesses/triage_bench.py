#!/usr/bin/env python3
"""Triage the corpus benchmark as it streams in: where is stanli losing,
and by how much. Reads the TSV corpus_bench.py writes. Current runs show
setup-plus-gradient estimates; older sampling artifacts remain readable.

Usage: python3 harnesses/triage_bench.py docs/corpus-bench.tsv [--all]
"""
import csv
import pathlib
import sys


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def main():
    rows = []
    path = pathlib.Path(sys.argv[1])
    with path.open(newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        estimated = "gradient_budget" in (reader.fieldnames or [])
        rows.extend(reader)

    done = len(rows)
    losses, wins, flags = [], [], []
    grad_ratios = []
    for r in rows:
        sg, cg = num(r["stanli_ns_grad"]), num(r["cmdstan_ns_grad"])
        metric = "estimated_s" if estimated else "sample_s"
        ss, cs = num(r.get("stanli_" + metric)), num(r.get("cmdstan_" + metric))
        note = r["note"]
        if note:
            flags.append((r["model"], note))
        if sg and cg:
            paired = num(r.get("paired_speedup"))
            ratio = paired if paired is not None else cg / sg  # >1 means Stanli faster
            grad_ratios.append((ratio, r["model"], sg, cg, ss, cs))
            (wins if ratio >= 1 else losses).append(ratio)

    print(f"{done} models measured; {len(grad_ratios)} with both gradients")
    if estimated:
        print("  Estimated seconds: measured setup + 20,000 warm gradients (proxy, not HMC time)")
    else:
        print("  Historical sampling seconds: recorded full CLI runs")
    if grad_ratios:
        grad_ratios.sort()
        print(f"  stanli faster on {len(wins)}, slower on {len(losses)}")
        print("\nWORST (stanli slowest vs CmdStan):")
        metric_label = "estimate" if estimated else "sample"
        print(f"  {'model':40s} {'stanli ns':>11s} {'cmdstan ns':>11s} "
              f"{'grad':>7s} {'stanli s':>9s} {'cmdstan s':>9s} {metric_label:>8s}")
        show = grad_ratios if "--all" in sys.argv else grad_ratios[:12]
        for ratio, m, sg, cg, ss, cs in show:
            ratio_text = f"{cs / ss:.2f}x" if (ss and cs) else "-"
            stanli_text = f"{ss:.2f}" if ss is not None else "-"
            cmdstan_text = f"{cs:.2f}" if cs is not None else "-"
            print(f"  {m:40s} {sg:11.0f} {cg:11.0f} {ratio:6.2f}x "
                  f"{stanli_text:>9s} {cmdstan_text:>9s} {ratio_text:>8s}")
        if "--all" not in sys.argv and len(grad_ratios) > 12:
            print(f"  ... and {len(grad_ratios) - 12} more (pass --all)")
    if flags:
        print("\nNOTES (failures / timeouts):")
        for m, n in flags:
            print(f"  {m:40s} {n}")


if __name__ == "__main__":
    main()
