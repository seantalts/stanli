#!/usr/bin/env python3
"""Stamp the measured numbers into the docs, so they cannot go stale.

Every headline number in README.md, python/README.md and the demo page
(counts, bitwise counts, worst deviation, benchmark span, the PyPI page's
benchmark table) is derived from three artifacts:

  docs/verification.json   written by tools/verify_sample.py
  docs/corpus-bench.tsv    written by harnesses/corpus_bench.py
  docs/benchmarks.md       chooses the representative benchmark rows

The docs carry <!--gen:key-->...<!--/gen--> markers; this script replaces
the marked spans with values computed from the artifacts.

  tools/gen_docs.py           rewrite the docs in place
  tools/gen_docs.py --check   exit 1 if any doc disagrees with the
                              artifacts (CI runs this)

The prose around the markers is hand-written; only the numbers move.
"""
import csv
import json
import pathlib
import re
import subprocess
import sys
import warnings

REPO = pathlib.Path(__file__).resolve().parent.parent
# The demo page carries headline numbers too, and its markers are HTML
# comments, so the same substitution works there.
TARGETS = [REPO / "README.md", REPO / "python" / "README.md",
           REPO / "web" / "index.html"]
MARK = re.compile(r"(<!--gen:([a-z_]+)-->)(.*?)(<!--/gen-->)", re.S)


def bench_rows():
    """Representative (model, stanli_ns, cmdstan_ns, speedup)."""
    with (REPO / "docs" / "corpus-bench.tsv").open(newline="") as f:
        corpus = {row["model"]: row for row in csv.DictReader(f, delimiter="\t")}
    text = (REPO / "docs" / "benchmarks.md").read_text()
    section = text.split("## Representative models", 1)[1]
    rows = []
    # Only the first table in the section. The benchmark page owns the
    # editorial choice of representative models; the TSV owns every measured
    # value.
    for line in section.splitlines():
        if not line.startswith("|"):
            if rows:
                break
            continue
        if not line.startswith("| `"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        model = cells[0].strip("`")
        if model not in corpus:
            raise SystemExit(f"representative benchmark missing from TSV: {model}")
        measured = corpus[model]
        stanli_ns = float(measured["stanli_ns_grad"])
        cmdstan_ns = float(measured["cmdstan_ns_grad"])
        rows.append((model, stanli_ns, cmdstan_ns, cmdstan_ns / stanli_ns))
    if not rows:
        raise SystemExit("no benchmark table found in docs/benchmarks.md")
    return rows


def corpus_stats():
    """(n, median speedup, models at or above parity) from the corpus TSV.

    The benchmark table above is a shape slice, chosen to show the range;
    summarizing the engine from it would be summarizing the choice. These
    are every model the corpus run measured on both sides.
    """
    rows = (REPO / "docs" / "corpus-bench.tsv").read_text().splitlines()
    idx = {name: k for k, name in enumerate(rows[0].split("\t"))}
    ratios = []
    for line in rows[1:]:
        c = line.split("\t")
        s, cm = c[idx["stanli_ns_grad"]], c[idx["cmdstan_ns_grad"]]
        if s.strip() and cm.strip():
            ratios.append(float(cm) / float(s))
    ratios.sort()
    return len(ratios), ratios[len(ratios) // 2], sum(r >= 1.0 for r in ratios)


def us(ns):
    v = ns / 1000.0
    return f"{v:.2f} us" if v < 1 else f"{v:.1f} us"


def compute():
    ver = json.loads((REPO / "docs" / "verification.json").read_text())
    # The same record holds two corpora, and the sentences about them say
    # different things: posteriordb is real posteriors, tests/stanc3 is
    # language constructs no posterior happens to use. Split them here so
    # the posteriordb numbers stay posteriordb numbers.
    lang = {k: v for k, v in ver.items()
            if (REPO / "tests" / "stanc3" / f"{k}.stan").exists()}
    ver = {k: v for k, v in ver.items() if k not in lang}
    verified = {k: v for k, v in ver.items() if v["status"] == "VERIFIED"}
    bitwise = sum(1 for v in verified.values() if v["max_ulp"] == 0)
    worst = max(v["max_rel"] for v in verified.values())
    n_total = len(ver)
    lang_verified = sum(1 for v in lang.values() if v["status"] == "VERIFIED")

    c_n, c_med, c_par = corpus_stats()
    rows = bench_rows()
    # A model counts as a win when its speedup rounds to at least 1.0x,
    # matching how the table has always been summarized.
    wins = [r for r in rows if round(r[3], 1) >= 1.0]
    losses = [r for r in rows if round(r[3], 1) < 1.0]
    span = (f"{min(round(r[3], 1) for r in rows):.1f}x-"
            f"{max(round(r[3], 1) for r in rows):.1f}x")
    loss_strs = [f"{r[3]:.2f}x" for r in sorted(losses, key=lambda r: -r[3])]
    loss_text = (" and ".join(loss_strs) if len(loss_strs) <= 2
                 else ", ".join(loss_strs[:-1]) + ", and " + loss_strs[-1])

    table = ["| model | stanli | CmdStan | speedup |",
             "| --- | ---: | ---: | ---: |"]
    for name, sns, cns, sp in rows:
        spd = f"**{sp:.1f}x**" if round(sp, 1) >= 1.0 else f"{sp:.2f}x"
        table.append(f"| `{name}` | {us(sns)} | {us(cns)} | {spd} |")

    return {
        "corpus_verified": f"{len(verified)}/{n_total}",
        "corpus_verified_of": f"{len(verified)} of {n_total}",
        "corpus_verified_n": str(len(verified)),
        "corpus_bitwise": str(bitwise),
        "corpus_worst": f"{worst:.1e}".replace("e-0", "e-"),
        "lang_verified": f"{lang_verified}/{len(lang)}",
        "lang_n": str(len(lang)),
        "bench_span": span,
        "bench_wins": f"{len(wins)} of the {len(rows)}",
        "bench_losses": loss_text,
        "bench_table_us": "\n".join(table),
        "corpus_median": f"{c_med:.2f}x",
        "corpus_n_grad": str(c_n),
        "corpus_at_par": str(c_par),
    }


def render_problems(path):
    """Markdown tables that PyPI would not render as tables.

    PyPI renders the long description with readme_renderer, which is
    stricter than GitHub about one thing that bit this file: a line
    starting with `<!--` opens a raw HTML block, and everything up to
    the `-->` is HTML, not markdown. A generated-value marker sitting on
    the same line as a table header therefore swallowed the header, and
    the delimiter row plus every data row rendered as literal pipes.
    twine check does not catch it: the page renders, it just renders
    wrong. Returns [] when readme_renderer[md] is not installed, so this
    is a CI check that a local run without it simply skips.
    """
    try:
        import readme_renderer.markdown
    except ImportError:
        return []
    text = path.read_text(encoding="utf-8")
    # A delimiter row, which is what makes the block above it a table:
    # every cell is dashes, optionally colon-anchored.
    delim = re.compile(r"\s*\|(\s*:?-+:?\s*\|)+\s*$")
    want = sum(1 for line in text.splitlines() if delim.match(line))
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        html = readme_renderer.markdown.render(text)
    if html is None:
        # readme_renderer is present but its markdown extra is not, so
        # it renders nothing at all. Not a finding about the docs.
        return []
    got = html.count("<table")
    if got >= want:
        return []
    rel = path.relative_to(REPO)
    return [f"{rel}: {want} markdown table(s), "
            f"{got} rendered by readme_renderer (PyPI would show "
            f"literal pipes)"]


def benchmark_table_problems():
    """Require the hand-edited benchmark tables to match their generator."""
    page = (REPO / "docs" / "benchmarks.md").read_text()
    generated = subprocess.check_output(
        [sys.executable, str(REPO / "tools" / "corpus_table.py"),
         str(REPO / "docs" / "corpus-bench.tsv")], text=True)

    def first_table(section):
        table = []
        for line in section.splitlines():
            if line.startswith("|"):
                table.append(line)
            elif table:
                break
        return table

    generated_main = first_table(generated)
    generated_stuck = first_table(generated.split(
        "| model | stanli gradient | CmdStan gradient | gradient speedup | "
        "what stopped it |", 1)[1])
    generated_stuck.insert(
        0, "| model | stanli gradient | CmdStan gradient | gradient speedup | "
           "what stopped it |")

    full = page.split("## Full corpus", 1)[1].split(
        "### Runs that did not complete", 1)[0]
    stuck = page.split("### Runs that did not complete", 1)[1].split(
        "## Benchmark method", 1)[0]
    problems = []
    if first_table(full) != generated_main:
        problems.append("docs/benchmarks.md: full corpus table is stale")
    if first_table(stuck) != generated_stuck:
        problems.append("docs/benchmarks.md: incomplete-runs table is stale")

    representative = first_table(page.split("## Representative models", 1)[1])
    generated_by_model = {
        line.split("|")[1].strip(): [c.strip() for c in line.strip("|").split("|")]
        for line in generated_main if line.startswith("| `")
    }
    for line in representative[2:]:
        cells = [c.strip() for c in line.strip("|").split("|")]
        source = generated_by_model.get(cells[0])
        expected = ([source[i] for i in (0, 1, 2, 3, 4, 6, 7)]
                    if source else None)
        if cells != expected:
            problems.append(
                f"docs/benchmarks.md: representative row {cells[0]} is stale")
    return problems


def main():
    check = "--check" in sys.argv
    stats = compute()
    stale = []
    for path in TARGETS:
        text = path.read_text()
        rel = path.relative_to(REPO)

        def sub(m):
            key = m.group(2)
            if key not in stats:
                raise SystemExit(f"{rel}: unknown marker gen:{key}")
            value = stats[key]
            # A multi-line value is a markdown block, and a block cannot
            # begin on the marker's own line: `<!--` opens a raw HTML
            # block that runs through the `-->`, so a table header
            # sharing that line is eaten as HTML and the rows below it
            # render as literal pipes (see render_problems).
            block = "\n" in value
            had = m.group(3).strip("\n") if block else m.group(3)
            if had != value:
                short = (value[:40] + "...") if len(value) > 40 else value
                stale.append(f"{rel}: {key}: {had[:40]!r} -> {short!r}")
            body = f"\n{value}\n" if block else value
            return m.group(1) + body + m.group(4)

        new = MARK.sub(sub, text)
        if not check and new != text:
            path.write_text(new)
    broken = [p for path in TARGETS for p in render_problems(path)]
    if broken:
        print("the rendered page would be wrong:")
        for b in broken:
            print(" ", b)
        return 1
    benchmark_broken = benchmark_table_problems()
    if benchmark_broken:
        print("benchmark tables disagree with docs/corpus-bench.tsv "
              "(run tools/corpus_table.py):")
        for b in benchmark_broken:
            print(" ", b)
        return 1
    if check and stale:
        print("docs disagree with the measured artifacts "
              "(run tools/gen_docs.py):")
        for s in stale:
            print(" ", s)
        return 1
    for s in stale:
        print("updated", s)
    if not stale:
        print("docs already match the artifacts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
