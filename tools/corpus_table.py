#!/usr/bin/env python3
"""Render docs/corpus-bench.tsv as the markdown tables docs/benchmarks.md
embeds.

Two tables. The first is every model both engines measured end to end,
sorted by per-gradient speedup: CmdStan's absolute times and stanli's
ratio against them, since the ratio is what the table is read for and
stanli's own numbers are the ratio times the CmdStan column. The second
is the models the run could not complete, with what stopped it -- they
sort to the bottom because a missing number is not a slow number.

Usage: python3 tools/corpus_table.py docs/corpus-bench.tsv
Prints markdown to stdout; benchmarks.md is edited by hand around it.
"""
import csv
import sys

# The harness's machine tags, in the words a reader needs. The per-model
# detail (why a model with a fast gradient still fails to sample inside
# the cap) is hand-written prose in benchmarks.md, next to the table.
WHY = {
    "stanli_sample_timeout": "stanli sampling hit the 900 s cap",
    "cmdstan_sample_timeout": "CmdStan sampling hit the 900 s cap",
    "stanli_eval_fail": "stanli's gradient probe threw at the benchmark point",
    "stanc_fail": "stanc could not compile the model",
    "cmdstan_build_fail": "CmdStan could not build the model",
    "cmdstan_grad_build_fail": "the CmdStan gradient driver would not link",
    "cmdstan_grad_fail": "the CmdStan gradient driver would not run",
}


def fmt_ns(v):
    return f"{int(float(v)):,}" if v else "-"


def ratio(a, b):
    """b over a as a speedup string: how much faster stanli is."""
    if not a or not b:
        return "-"
    return f"{float(b) / float(a):.2f}x"


def main():
    rows = []
    with open(sys.argv[1], newline="") as f:
        reader = csv.reader(f, delimiter="\t")
        header = next(reader)
        idx = {name: k for k, name in enumerate(header)}
        for c in reader:
            if len(c) < len(header):
                c += [""] * (len(header) - len(c))
            rows.append(c)

    def col(r, name):
        return r[idx[name]].strip()

    def grad_ratio(r):
        a, b = col(r, "stanli_ns_grad"), col(r, "cmdstan_ns_grad")
        return float(b) / float(a) if a and b else -1.0

    def why(r):
        """Why this row is incomplete, or "" if it is not."""
        reasons = [WHY.get(n, n) for n in col(r, "note").split(",") if n]
        if not col(r, "stanli_ns_grad"):
            reasons.append("no stanli gradient")
        if not col(r, "cmdstan_ns_grad") and "eval_fail" not in col(r, "note"):
            reasons.append("no CmdStan gradient")
        return "; ".join(dict.fromkeys(reasons))

    done = [r for r in rows if not why(r)]
    stuck = [r for r in rows if why(r)]
    done.sort(key=grad_ratio, reverse=True)
    stuck.sort(key=grad_ratio, reverse=True)

    print("| model | params | CmdStan ns/grad | grad speedup |"
          " CmdStan sample | sample speedup |")
    print("| --- | ---: | ---: | ---: | ---: | ---: |")
    for r in done:
        cs = col(r, "cmdstan_sample_s")
        print(f"| `{col(r, 'model')}` | {col(r, 'params')} "
              f"| {fmt_ns(col(r, 'cmdstan_ns_grad'))} "
              f"| {ratio(col(r, 'stanli_ns_grad'), col(r, 'cmdstan_ns_grad'))} "
              f"| {cs + ' s' if cs else '-'} "
              f"| {ratio(col(r, 'stanli_sample_s'), cs)} |")

    ratios = sorted(g for g in (grad_ratio(r) for r in rows) if g > 0)
    at_par = sum(1 for g in ratios if g >= 1.0)
    med = ratios[len(ratios) // 2] if ratios else 0
    print()
    print(f"{len(rows)} models; {len(ratios)} with both gradients; median "
          f"per-gradient speedup {med:.2f}x; {at_par}/{len(ratios)} at or "
          f"above CmdStan.")

    if not stuck:
        return
    print()
    print("| model | params | CmdStan ns/grad | grad speedup | what stopped it |")
    print("| --- | ---: | ---: | ---: | --- |")
    for r in stuck:
        print(f"| `{col(r, 'model')}` | {col(r, 'params')} "
              f"| {fmt_ns(col(r, 'cmdstan_ns_grad'))} "
              f"| {ratio(col(r, 'stanli_ns_grad'), col(r, 'cmdstan_ns_grad'))} "
              f"| {why(r)} |")


if __name__ == "__main__":
    main()
