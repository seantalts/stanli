#!/usr/bin/env python3
"""Stamp the measured numbers into the docs, so they cannot go stale.

Every headline number in README.md, python/README.md and the demo page
(counts, bitwise counts, worst deviation, and current benchmark summaries)
is derived from recorded artifacts:

  docs/verification.json   written by tools/verify_sample.py
  output/corpus-performance/  one current full-corpus benchmark

The docs carry <!--gen:key-->...<!--/gen--> markers; this script replaces
the marked spans with values computed from the artifacts.

  tools/gen_docs.py           rewrite the docs in place
  tools/gen_docs.py --check   exit 1 if any doc disagrees with the
                              artifacts (CI runs this)

The prose around the markers is hand-written; only the numbers move.
"""
import csv
import gzip
import json
import pathlib
import re
import statistics
import subprocess
import sys
import warnings

from corpus_table import render_catalog, render_gradient_catalog

REPO = pathlib.Path(__file__).resolve().parent.parent
# The demo page carries headline numbers too, and its markers are HTML
# comments, so the same substitution works there.
TARGETS = [REPO / "README.md", REPO / "python" / "README.md",
           REPO / "web" / "index.html", REPO / "tests" / "rethinking" / "README.md",
           REPO / "docs" / "benchmarks.md", REPO / "docs" / "benchmark-appendix.md"]
MARK = re.compile(r"(<!--gen:([a-z_]+)-->)(.*?)(<!--/gen-->)", re.S)


def current_benchmark():
    """Validated current table, rows, and manifest shared by public summaries.

    Validate the complete inventory, provenance, and measurement evidence
    before deriving any headline or browser-card value. Never fall back to
    historical measurements when a current artifact is missing or invalid.
    """
    directory = REPO / "output" / "corpus-performance"
    summary = directory / "benchmark-summary.tsv"
    records = directory / "model-results.json.gz"
    manifest_path = directory / "benchmark-manifest.json"
    catalog = render_catalog(summary, records, manifest_path).rstrip()
    with summary.open(newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    manifest = json.loads(manifest_path.read_text())
    return catalog, rows, manifest


def corpus_stats(rows):
    """Current per-model paired ratios; all source collections participate."""
    ratios = [float(row["paired_speedup"]) for row in rows if row.get("paired_speedup")]
    return (len(ratios), statistics.median(ratios) if ratios else None,
            sum(ratio >= 1.0 for ratio in ratios))


def compute():
    ver = json.loads((REPO / "docs" / "verification.json").read_text())
    # The same record holds three corpora, and the sentences about them
    # say different things: posteriordb is real posteriors, tests/stanc3
    # is language constructs no posterior happens to use, tests/brms is
    # brms output. Split them here so the posteriordb numbers stay
    # posteriordb numbers.
    lang = {k: v for k, v in ver.items()
            if (REPO / "tests" / "stanc3" / f"{k}.stan").exists()}
    brms = {k: v for k, v in ver.items()
            if (REPO / "tests" / "brms" / f"{k}.stan").exists()}
    rethinking = {p.stem for p in (REPO / "tests" / "rethinking").glob("*.stan")}
    rethinking_verified = sum(ver.get(k, {}).get("status") == "VERIFIED"
                             for k in rethinking)
    imported = {p.name for p in (REPO / "tests/educational/models").iterdir() if p.is_dir()}
    ver = {k: v for k, v in ver.items()
           if k not in lang and k not in brms and k not in rethinking and k not in imported}
    references = json.loads(gzip.decompress((REPO / "docs/corpus-refs.json.gz").read_bytes()))["models"]
    verified = {k: v for k, v in ver.items() if v["status"] == "VERIFIED"}
    bitwise = sum(1 for v in verified.values() if v["max_ulp"] == 0)
    worst = max(v["max_rel"] for v in verified.values())
    n_total = len(ver)
    lang_verified = sum(1 for v in lang.values() if v["status"] == "VERIFIED")

    catalog, benchmark_rows, manifest = current_benchmark()
    c_n, c_med, c_par = corpus_stats(benchmark_rows)

    return {
        "benchmark_catalog": catalog,
        "benchmark_models": str(len(manifest["identity"]["inputs"])),
        "benchmark_date": manifest["started_utc"][:10],
        "benchmark_vectorized_catalog": render_gradient_catalog().rstrip(),
        "corpus_reference_models": str(len(references)),
        "corpus_reference_points": str(sum(len(row["points"]) for row in references.values())),
        "corpus_verified": f"{len(verified)}/{n_total}",
        "corpus_verified_of": f"{len(verified)} of {n_total}",
        "corpus_verified_n": str(len(verified)),
        "corpus_bitwise": str(bitwise),
        "corpus_worst": f"{worst:.1e}".replace("e-0", "e-"),
        "lang_verified": f"{lang_verified}/{len(lang)}",
        "lang_n": str(len(lang)),
        "rethinking_n": str(len(rethinking)),
        "rethinking_verified": f"{rethinking_verified}/{len(rethinking)}",
        "corpus_median": f"{c_med:.2f}x" if c_med is not None else "unavailable",
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
