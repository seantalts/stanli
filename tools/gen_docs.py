#!/usr/bin/env python3
"""Stamp the measured numbers into the docs, so they cannot go stale.

Every headline number in README.md and python/README.md (verified model
counts, bitwise counts, worst deviation, benchmark span, the PyPI page's
benchmark table) is derived from two artifacts:

  docs/verification.json   written by tools/verify_sample.py
  docs/benchmarks.md       per-gradient table, written when benchmarks run

The docs carry <!--gen:key-->...<!--/gen--> markers; this script replaces
the marked spans with values computed from the artifacts.

  tools/gen_docs.py           rewrite the docs in place
  tools/gen_docs.py --check   exit 1 if any doc disagrees with the
                              artifacts (CI runs this)

The prose around the markers is hand-written; only the numbers move.
"""
import json
import pathlib
import re
import sys
import warnings

REPO = pathlib.Path(__file__).resolve().parent.parent
TARGETS = [REPO / "README.md", REPO / "python" / "README.md"]
MARK = re.compile(r"(<!--gen:([a-z_]+)-->)(.*?)(<!--/gen-->)", re.S)


def num(cell):
    """A table cell as a number. Thousands separators are for readers."""
    return float(cell.replace(",", "").rstrip("x"))


def bench_rows():
    """(model, params, stanli_ns, cmdstan_ns, speedup) from benchmarks.md."""
    text = (REPO / "docs" / "benchmarks.md").read_text()
    section = text.split("## Per-gradient latency")[1]
    rows = []
    # Only the first table in the section: stop at the first non-table
    # line after rows begin (the ODE section further down has its own
    # before/after table this must not swallow).
    for line in section.splitlines():
        if not line.startswith("|"):
            if rows:
                break
            continue
        if not line.startswith("| `"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        rows.append((cells[0].strip("`"), int(num(cells[1])), num(cells[2]),
                     num(cells[3]), num(cells[4])))
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
    verified = {k: v for k, v in ver.items() if v["status"] == "VERIFIED"}
    bitwise = sum(1 for v in verified.values() if v["max_ulp"] == 0)
    worst = max(v["max_rel"] for v in verified.values())
    n_total = len(ver)

    c_n, c_med, c_par = corpus_stats()
    rows = bench_rows()
    # A model counts as a win when its speedup rounds to at least 1.0x,
    # matching how the table has always been summarized.
    wins = [r for r in rows if round(r[4], 1) >= 1.0]
    losses = [r for r in rows if round(r[4], 1) < 1.0]
    span = (f"{min(round(r[4], 1) for r in wins):.1f}x-"
            f"{max(round(r[4], 1) for r in wins):.1f}x")
    loss_strs = [f"{r[4]:.2f}x" for r in sorted(losses, key=lambda r: -r[4])]
    loss_text = (" and ".join(loss_strs) if len(loss_strs) <= 2
                 else ", ".join(loss_strs[:-1]) + ", and " + loss_strs[-1])

    table = ["| model | params | stanli | CmdStan | speedup |",
             "| --- | ---: | ---: | ---: | ---: |"]
    for name, p, sns, cns, sp in rows:
        spd = f"**{sp:.1f}x**" if round(sp, 1) >= 1.0 else f"{sp:.2f}x"
        table.append(f"| `{name}` | {p} | {us(sns)} | {us(cns)} | {spd} |")

    return {
        "corpus_verified": f"{len(verified)}/{n_total}",
        "corpus_verified_of": f"{len(verified)} of {n_total}",
        "corpus_verified_n": str(len(verified)),
        "corpus_bitwise": str(bitwise),
        "corpus_worst": f"{worst:.1e}".replace("e-0", "e-"),
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
