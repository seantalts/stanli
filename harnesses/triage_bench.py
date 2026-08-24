#!/usr/bin/env python3
"""Triage the corpus benchmark as it streams in: where is stanli losing,
and by how much. Reads the TSV corpus_bench.py writes.

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
        rows.extend(csv.DictReader(f, delimiter="\t"))

    done = len(rows)
    losses, wins, flags = [], [], []
    grad_ratios = []
    for r in rows:
        sg, cg = num(r["stanli_ns_grad"]), num(r["cmdstan_ns_grad"])
        ss, cs = num(r["stanli_sample_s"]), num(r["cmdstan_sample_s"])
        note = r["note"]
        if note:
            flags.append((r["model"], note))
        if sg and cg:
            ratio = cg / sg          # >1 means stanli faster
            grad_ratios.append((ratio, r["model"], sg, cg, ss, cs))
            (wins if ratio >= 1 else losses).append(ratio)

    print(f"{done} models measured; {len(grad_ratios)} with both gradients")
    if grad_ratios:
        grad_ratios.sort()
        print(f"  stanli faster on {len(wins)}, slower on {len(losses)}")
        print("\nWORST (stanli slowest vs CmdStan):")
        print(f"  {'model':40s} {'stanli ns':>11s} {'cmdstan ns':>11s} "
              f"{'grad':>7s} {'stanli s':>9s} {'cmdstan s':>9s} {'sample':>7s}")
        show = grad_ratios if "--all" in sys.argv else grad_ratios[:12]
        for ratio, m, sg, cg, ss, cs in show:
            samp = f"{cs / ss:.2f}x" if (ss and cs) else "-"
            print(f"  {m:40s} {sg:11.0f} {cg:11.0f} {ratio:6.2f}x "
                  f"{ss if ss else 0:9.2f} {cs if cs else 0:9.2f} {samp:>7s}")
        if "--all" not in sys.argv and len(grad_ratios) > 12:
            print(f"  ... and {len(grad_ratios) - 12} more (pass --all)")
    if flags:
        print("\nNOTES (failures / timeouts):")
        for m, n in flags:
            print(f"  {m:40s} {n}")


if __name__ == "__main__":
    main()
