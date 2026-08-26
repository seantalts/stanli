#!/usr/bin/env python3
"""Render docs/corpus-bench.tsv as the markdown tables docs/benchmarks.md
embeds.

Two tables. The first is every model both engines measured end to end,
sorted by per-gradient speedup. It shows both engines' absolute gradient
times and the wall time from Stan source to a completed 1,000-warmup,
1,000-draw run. stanli_sample_s already includes the whole stanli process;
CmdStan's equivalent first-run time is its separately measured build plus
run. The second table holds models the run could not complete, with what
stopped them. Missing numbers sort to the bottom because missing is not slow.

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
    if not v:
        return "-"
    ns = float(v)
    if ns < 1_000:
        return f"{ns:.0f} ns"
    if ns < 1_000_000:
        return f"{ns / 1_000:.3f} us"
    return f"{ns / 1_000_000:.3f} ms"


def fmt_s(v):
    return f"{float(v):.2f} s" if v else "-"


def fmt_cmdstan_s(v):
    """CmdStan build inputs are recorded to 0.1 s, so totals are too."""
    return f"{float(v):.1f} s" if v else "-"


def ratio(a, b):
    """b over a as a speedup string: how much faster stanli is."""
    if not a or not b:
        return "-"
    return f"{float(b) / float(a):.2f}x"


def first_run_ratio(r, col):
    stanli = col(r, "stanli_sample_s")
    build = col(r, "cmdstan_build_s")
    sample = col(r, "cmdstan_sample_s")
    if not stanli or not build or not sample:
        return None
    return (float(build) + float(sample)) / float(stanli)


def fmt_first_run_ratio(value):
    """Do not imply more precision than the 0.01/0.1 s source cells."""
    if value < 1:
        return f"~{value:.2f}x"
    if value < 10:
        return f"~{value:.1f}x"
    if value < 100:
        return f"~{value:.0f}x"
    return f"~{round(value, -1):.0f}x"


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

    print("| model | stanli gradient | CmdStan gradient | gradient speedup |"
          " stanli source-to-CSV | CmdStan build | CmdStan build + run |"
          " approx. first-run speedup |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for r in done:
        cs_total = (float(col(r, "cmdstan_build_s"))
                    + float(col(r, "cmdstan_sample_s")))
        print(f"| `{col(r, 'model')}` "
              f"| {fmt_ns(col(r, 'stanli_ns_grad'))} "
              f"| {fmt_ns(col(r, 'cmdstan_ns_grad'))} "
              f"| {ratio(col(r, 'stanli_ns_grad'), col(r, 'cmdstan_ns_grad'))} "
              f"| {fmt_s(col(r, 'stanli_sample_s'))} "
              f"| {fmt_cmdstan_s(col(r, 'cmdstan_build_s'))} "
              f"| {fmt_cmdstan_s(cs_total)} "
              f"| {fmt_first_run_ratio(first_run_ratio(r, col))} |")

    ratios = sorted(g for g in (grad_ratio(r) for r in rows) if g > 0)
    at_par = sum(1 for g in ratios if g >= 1.0)
    med = ratios[len(ratios) // 2] if ratios else 0
    first_runs = sorted(x for x in (first_run_ratio(r, col) for r in rows)
                        if x is not None)
    first_at_par = sum(1 for x in first_runs if x >= 1.0)
    first_med = first_runs[len(first_runs) // 2] if first_runs else 0
    print()
    print(f"{len(rows)} models; {len(ratios)} with both gradients; median "
          f"per-gradient speedup {med:.2f}x; {at_par}/{len(ratios)} at or "
          f"above CmdStan.")
    print(f"{len(first_runs)} completed first runs; median source-to-CSV "
          f"speedup about {first_med:.1f}x; {first_at_par}/{len(first_runs)} at or "
          f"above CmdStan including its model build.")

    if not stuck:
        return
    print()
    print("| model | stanli gradient | CmdStan gradient | gradient speedup |"
          " what stopped it |")
    print("| --- | ---: | ---: | ---: | --- |")
    for r in stuck:
        print(f"| `{col(r, 'model')}` "
              f"| {fmt_ns(col(r, 'stanli_ns_grad'))} "
              f"| {fmt_ns(col(r, 'cmdstan_ns_grad'))} "
              f"| {ratio(col(r, 'stanli_ns_grad'), col(r, 'cmdstan_ns_grad'))} "
              f"| {why(r)} |")


if __name__ == "__main__":
    main()
