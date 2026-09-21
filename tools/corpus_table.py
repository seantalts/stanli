#!/usr/bin/env python3
"""Render the benchmark catalog and detailed historical appendix tables.

--catalog renders one complete, current benchmark run for benchmarks.md.
The original TSV mode emits two tables for docs/benchmark-appendix.md. The first is every model both engines measured end to end,
sorted by per-gradient speedup. It shows both engines' absolute gradient
times and the wall time from Stan source to a completed 1,000-warmup,
1,000-draw run. stanli_sample_s already includes the whole stanli process;
CmdStan's equivalent first-run time is its separately measured build plus
run. The second table holds models the run could not complete, with what
stopped them. Missing numbers sort to the bottom because missing is not slow.

Usage: python3 tools/corpus_table.py docs/corpus-bench.tsv
Prints markdown to stdout; the appendix is edited by hand around it.

--catalog requires the current full-corpus summary, diagnostics, and manifest under
output/corpus-performance; it never substitutes historical measurements.

--gradients INPUT.tsv renders only the fixed-point gradient comparison.

--historical-sampling REPORT.json renders the archived paired sampling experiment:
python3 tools/corpus_table.py --historical-sampling tests/educational/pareto-benchmark-results.json
The old --educational spelling remains an alias for archived commands.

--o1vec renders docs/corpus-bench-o1vec.tsv instead: gradient and compile
time only, no sampling columns, plus a compile+sample speedup that adds
2,000 synthetic gradient evaluations to each side's compile time. Usage:
python3 tools/corpus_table.py docs/corpus-bench-o1vec.tsv --o1vec
"""
import csv
import re
import json
import math
import statistics
from pathlib import Path
import sys

# The harness's machine tags, in the words a reader needs. The per-model
# detail (why a model with a fast gradient still fails to sample inside
# the cap) is hand-written prose in benchmarks.md, next to the table.
WHY = {
    "stanli_sample_timeout": "stanli sampling hit the 900 s cap",
    "cmdstan_sample_timeout": "CmdStan sampling hit the 900 s cap",
    "stanli_eval_fail": "stanli's gradient probe threw at the benchmark point",
    "stanli_prep_fail": "stanli's compile-and-bind step failed",
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


def fmt_prep_s(v):
    """stanli's prep column is recorded to 0.001 s, so keep that precision."""
    return f"{float(v):.3f} s" if v else "-"


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


def two_k_grad_ratio(r, col):
    """Compile time plus 2,000 gradients: the cost of a first short run."""
    prep = col(r, "stanli_prep_s")
    s_grad = col(r, "stanli_ns_grad")
    build = col(r, "cmdstan_build_s")
    c_grad = col(r, "cmdstan_ns_grad")
    if not prep or not s_grad or not build or not c_grad:
        return None
    stanli_total = float(prep) + 2000 * float(s_grad) / 1e9
    cmdstan_total = float(build) + 2000 * float(c_grad) / 1e9
    return cmdstan_total / stanli_total


def load_rows(path):
    with open(path, newline="") as f:
        reader = csv.reader(f, delimiter="\t")
        header = next(reader)
        idx = {name: k for k, name in enumerate(header)}
        rows = []
        for c in reader:
            if len(c) < len(header):
                c += [""] * (len(header) - len(c))
            rows.append(c)

    def col(r, name):
        return r[idx[name]].strip()

    col.columns = set(idx)
    return rows, col


def render_o1vec(rows, col):
    relevant_notes = {"stanc_fail", "stanli_eval_fail", "stanli_prep_fail",
                       "cmdstan_build_fail", "cmdstan_grad_build_fail",
                       "cmdstan_grad_fail"}

    def grad_ratio(r):
        a, b = col(r, "stanli_ns_grad"), col(r, "cmdstan_ns_grad")
        return float(b) / float(a) if a and b else -1.0

    def why(r):
        """Why this row is incomplete, or "" if it is not."""
        reasons = [WHY.get(n, n) for n in col(r, "note").split(",")
                   if n in relevant_notes]
        if not col(r, "stanli_prep_s"):
            reasons.append("no stanli compile time")
        if not col(r, "stanli_ns_grad"):
            reasons.append("no stanli gradient")
        if not col(r, "cmdstan_build_s"):
            reasons.append("no CmdStan compile time")
        if not col(r, "cmdstan_ns_grad") and "eval_fail" not in col(r, "note"):
            reasons.append("no CmdStan gradient")
        return "; ".join(dict.fromkeys(reasons))

    done = [r for r in rows if not why(r)]
    stuck = [r for r in rows if why(r)]
    done.sort(key=grad_ratio, reverse=True)
    stuck.sort(key=grad_ratio, reverse=True)

    print("| model | gradient speedup | stanli compile | CmdStan compile |"
          " compile+sample speedup |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for r in done:
        print(f"| `{col(r, 'model')}` "
              f"| {ratio(col(r, 'stanli_ns_grad'), col(r, 'cmdstan_ns_grad'))} "
              f"| {fmt_prep_s(col(r, 'stanli_prep_s'))} "
              f"| {fmt_cmdstan_s(col(r, 'cmdstan_build_s'))} "
              f"| {fmt_first_run_ratio(two_k_grad_ratio(r, col))} |")

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


def render_main(rows, col):
    def grad_ratio(r):
        if "paired_speedup" in col.columns:
            return float(col(r, "paired_speedup")) if col(r, "paired_speedup") else -1.0
        a, b = col(r, "stanli_ns_grad"), col(r, "cmdstan_ns_grad")
        return float(b) / float(a) if a and b else -1.0

    def why(r):
        """Why this row is incomplete, or "" if it is not."""
        reasons = [re.sub(r"^(stanli|cmdstan)_sample_timeout\((\d+)s\)$",
                          lambda m: f"{'CmdStan' if m[1] == 'cmdstan' else 'stanli'} "
                                    f"sampling hit the {m[2]} s cap",
                          WHY.get(n, n))
                   for n in re.split(r"[,;]\s*", col(r, "note")) if n]
        if not col(r, "stanli_ns_grad"):
            reasons.append("no stanli gradient")
        if not col(r, "cmdstan_ns_grad") and "eval_fail" not in col(r, "note"):
            reasons.append("no CmdStan gradient")
        return "; ".join(dict.fromkeys(reasons))

    if "paired_speedup" in col.columns:
        print("Gradient timings are medians ± MAD; speedups are medians of paired ratios.")
        print()
        print("| model | stanli gradient | CmdStan gradient | paired speedup | pairs | status |")
        print("| --- | ---: | ---: | ---: | ---: | --- |")
        for r in sorted(rows, key=grad_ratio, reverse=True):
            times = [f"{fmt_ns(col(r, e + '_ns_grad'))} ± {fmt_ns(col(r, e + '_ns_grad_mad'))}"
                     for e in ("stanli", "cmdstan")]
            speedup = (f"{grad_ratio(r):.2f}x ± {float(col(r, 'paired_speedup_mad')):.2f}"
                       if grad_ratio(r) > 0 else "-")
            status = col(r, "note").replace("sampling_not_requested", "gradients only") or "complete"
            print(f"| `{col(r, 'model')}` | {times[0]} | {times[1]} | {speedup} "
                  f"| {col(r, 'paired_rounds') or '-'} | {status} |")
        if any(col(r, "stanli_sample_s") or col(r, "cmdstan_sample_s") for r in rows):
            print("\n| model | stanli CLI median | CmdStan build | CmdStan CLI median |")
            print("| --- | ---: | ---: | ---: |")
            for r in rows:
                print(f"| `{col(r, 'model')}` | {fmt_s(col(r, 'stanli_sample_s'))} "
                      f"| {fmt_s(col(r, 'cmdstan_build_s'))} | {fmt_s(col(r, 'cmdstan_sample_s'))} |")
        return

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


def render_gradients(rows, col):
    """Fixed-point throughput; no build cost or sampler trajectory in the ratio."""
    print("| model | parameters | stanli gradient | CmdStan gradient | gradient speedup |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for row in sorted(rows, key=lambda r: col(r, "model")):
        a, b = col(row, "stanli_ns_grad"), col(row, "cmdstan_ns_grad")
        if (not a or not b or not all(math.isfinite(float(v)) and float(v) > 0
                                     for v in (a, b)) or col(row, "note")):
            raise ValueError(f"Incomplete gradient measurement: {col(row, 'model')}")
        print(f"| `{col(row, 'model')}` | {col(row, 'params')} "
              f"| {fmt_ns(a)} | {fmt_ns(b)} | {ratio(a, b)} |")


def render_historical_sampling(report):
    """Render the paired source-to-CSV gate without inventing gradient timings."""
    from historical_benchmarks import inventory, speed_gate, MINIMUM_SPEEDUPS
    if set(report["models"]) != set(inventory()):
        raise ValueError("Educational result inventory differs from the fixtures")
    print("| model | stanli source-to-CSV | compiled CmdStan run | speedup | required floor |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for model, entry in sorted(report["models"].items()):
        if "error" in entry:
            raise ValueError(f"Failed educational result: {model}")
        bench = entry["benchmark"]
        minimum = MINIMUM_SPEEDUPS.get(model, 0.5)
        measured = speed_gate(bench["seconds"], minimum)
        if not measured["pass"] or not bench["posterior_pass"]:
            raise ValueError(f"Educational gate did not pass: {model}")
        med = measured["medians"]
        print(f"| `{model}` | {med['stanli'] * 1000:.2f} ms "
              f"| {med['cmdstan'] * 1000:.2f} ms "
              f"| {measured['speedup']:.3f}x | {minimum:.1f}x |")


CATALOG_ROOT = Path(__file__).resolve().parents[1]
CATALOG_SUMMARY = CATALOG_ROOT / "output/corpus-performance/benchmark-summary.tsv"
CATALOG_REPORT = CATALOG_ROOT / "output/corpus-performance/corpus-results.json"
CATALOG_MANIFEST = CATALOG_ROOT / "output/corpus-performance/benchmark-manifest.json"


def _catalog_rows(path):
    with open(path, newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    names = [row["model"] for row in rows]
    if len(set(names)) != len(names):
        raise ValueError(f"Duplicate model in benchmark artifact: {path}")
    return rows


def _catalog_number(row, field):
    value = row.get(field, "")
    if value is None or str(value).strip() == "":
        return None
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise ValueError(f"Invalid {field} for {row['model']}: {value}")
    return number


def _catalog_agrees(model, field, actual, expected):
    if ((actual is None) != (expected is None)
            or (actual is not None and not math.isclose(actual, expected,
                                                        rel_tol=1e-12, abs_tol=1e-15))):
        raise ValueError(f"Benchmark artifacts disagree: {model} {field}")


def _catalog_cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def render_catalog(summary_path=CATALOG_SUMMARY, report_path=CATALOG_REPORT,
                   manifest_path=CATALOG_MANIFEST):
    """Return an alphabetical table from one complete full-corpus run.

    All three artifacts must identify the same run and complete manifest
    inventory. Reported CLI medians require every declared seed to finish;
    missing/capped measurements and current diagnostic flags stay visible.
    CmdStan model compilation is excluded from its CLI column. Historical
    artifacts are never used as a fallback when current outputs are absent.
    """
    rows = _catalog_rows(summary_path)
    with open(report_path) as stream:
        report = json.load(stream)
    with open(manifest_path) as stream:
        manifest = json.load(stream)
    run_id = manifest["run_id"]
    if not run_id or report["run_id"] != run_id or report["manifest"] != manifest:
        raise ValueError("Benchmark artifacts must identify the same run and manifest")
    config = manifest["identity"]["config"]
    if config.get("corpus") != "all" or config.get("filter") or not config.get("sampling"):
        raise ValueError("Catalog requires an unfiltered full-corpus sampling run")
    seeds = config["seeds"]
    if not seeds or len(seeds) != len(set(seeds)):
        raise ValueError("Benchmark manifest must declare distinct sampling seeds")
    expected_inputs = manifest["identity"]["inputs"]
    details = {row["model"]: row for row in report["rows"]}
    if (not expected_inputs or len(details) != len(report["rows"])
            or set(details) != set(expected_inputs)
            or {row["model"] for row in rows} != set(expected_inputs)):
        raise ValueError("Run is incomplete: summary/report inventories must match every manifest input")
    output = [
        f"Run `{_catalog_cell(run_id)}` ({_catalog_cell(manifest['started_utc'][:10])}): "
        f"{config['rounds']} paired gradient rounds and {len(seeds)} sampling seeds, "
        f"each with {config['iter_warmup']:,} warmup iterations and {config['iter_sampling']:,} retained draws.",
        "",
        "Gradient ratio is CmdStan/Stanli, reported as the median paired ratio ± MAD. "
        "CLI times are median seconds across all declared seeds, exclude CmdStan model "
        "compilation, and include Stanli source preparation. “Complete” describes execution, "
        "not convergence; diagnostic flags and failed or capped runs remain visible. "
        "Missing measurements are —.",
        "",
        "| Model | Gradient ratio | Stanli CLI (s) | CmdStan CLI (s) | Notes |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for row in sorted(rows, key=lambda item: item["model"]):
        model = row["model"]
        detail = details[model]
        if row.get("run_id") != run_id:
            raise ValueError(f"Benchmark run identities differ: {model}")
        for field in ("stan", "data"):
            value = expected_inputs[model].get(field)
            if not value or detail.get("inputs", {}).get(field) != value:
                raise ValueError(f"Benchmark input hashes differ: {model} {field}")
        note = row.get("note", "").strip()
        notes = [note] if note else []
        failure = detail.get("failure_reason", "")
        if failure and failure != note:
            notes.append(failure)
        if detail["status"] != "ok" and not notes:
            notes.append(detail["status"])
        times = {engine: _catalog_number(row, engine + "_sample_s")
                 for engine in ("stanli", "cmdstan")}
        speedup = _catalog_number(row, "paired_speedup")
        mad = _catalog_number(row, "paired_speedup_mad")
        for field in ("stanli_ns_grad", "cmdstan_ns_grad", "paired_speedup", "paired_speedup_mad"):
            value = _catalog_number(row, field)
            _catalog_agrees(model, field, value, detail.get("gradient", {}).get(field))
            if value == 0 and field != "paired_speedup_mad":
                raise ValueError(f"Zero gradient measurement for {model}: {field}")
        if (speedup is None) != (mad is None):
            raise ValueError(f"Incomplete paired gradient summary: {model}")
        if speedup is not None and int(row["paired_rounds"]) != config["rounds"]:
            raise ValueError(f"Gradient round count differs from manifest: {model}")
        gradient = f"{speedup:.2f}x ± {mad:.2f}" if speedup is not None else "—"
        for engine, label in (("stanli", "Stanli"), ("cmdstan", "CmdStan")):
            result = detail["engines"][engine]
            expected = result.get("median_s") if result["status"] == "complete" else None
            _catalog_agrees(model, engine + "_sample_s", times[engine], expected)
            if result["status"] == "complete":
                runs = [run for run in detail["runs"] if run["engine"] == engine]
                if (len(runs) != len(seeds) or {run['seed'] for run in runs} != set(seeds)
                        or any(run["status"] != "ok" for run in runs)):
                    raise ValueError(f"Incomplete sampling seeds cannot supply a CLI median: {model} {engine}")
                _catalog_agrees(model, engine + "_sample_s", times[engine],
                                statistics.median(run['elapsed_s'] for run in runs))
            diagnostics = result.get("diagnostics", {})
            if diagnostics.get("status") == "complete":
                if diagnostics.get("draws", 0) > 0 and diagnostics.get("divergences", 0) >= diagnostics['draws']:
                    notes.append(f"CLI comparison invalid: all retained {label} draws divergent")
                elif diagnostics.get("screening_flag"):
                    notes.append(label + " diagnostics flagged")
            elif result["status"] == "complete":
                notes.append(label + " diagnostics unavailable")
        if not notes and (any(value is None for value in times.values()) or gradient == "—"):
            notes.append("incomplete measurements")
        cells = [f"`{model}`", gradient,
                 *(f"{times[engine]:.4g}" if times[engine] is not None else "—"
                   for engine in ("stanli", "cmdstan")),
                 "; ".join(notes) or "complete"]
        output.append("| " + " | ".join(_catalog_cell(cell) for cell in cells) + " |")
    return "\n".join(output) + "\n"


def main():
    if "--catalog" in sys.argv:
        print(render_catalog(), end="")
        return
    if "--gradients" in sys.argv:
        path = sys.argv[sys.argv.index("--gradients") + 1]
        render_gradients(*load_rows(path))
        return
    if "--historical-sampling" in sys.argv or "--educational" in sys.argv:
        flag = "--historical-sampling" if "--historical-sampling" in sys.argv else "--educational"
        path = sys.argv[sys.argv.index(flag) + 1]
        with open(path) as stream:
            render_historical_sampling(json.load(stream))
        return
    o1vec = "--o1vec" in sys.argv
    path = [a for a in sys.argv[1:] if a != "--o1vec"][0]
    rows, col = load_rows(path)
    if o1vec:
        render_o1vec(rows, col)
    else:
        render_main(rows, col)


if __name__ == "__main__":
    main()
