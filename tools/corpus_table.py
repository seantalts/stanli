#!/usr/bin/env python3
"""Render the benchmark catalog and detailed historical appendix tables.

--catalog renders one complete, current benchmark run for benchmarks.md.
The original TSV mode emits two tables for the archived September 11 run
(notes/performance/2026-09-11-posteriordb-benchmark.md). The first is every model both engines measured end to end,
sorted by per-gradient speedup. It shows both engines' absolute gradient
times and the wall time from Stan source to a completed 1,000-warmup,
1,000-draw run. stanli_sample_s already includes the whole stanli process;
CmdStan's equivalent first-run time is its separately measured build plus
run. The second table holds models the run could not complete, with what
stopped them. Missing numbers sort to the bottom because missing is not slow.

Usage: python3 tools/corpus_table.py docs/corpus-bench.tsv
Prints markdown to stdout.

--catalog requires the current full-corpus v4 summary, raw model records and
manifest under output/corpus-performance; it never substitutes historical measurements.
Its setup-plus-20,000-gradients estimate is a fixed-work proxy, not measured sampling.

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
import gzip
import shlex
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
        if "stanli_sample_s" in col.columns and any(
                col(r, "stanli_sample_s") or col(r, "cmdstan_sample_s") for r in rows):
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
CATALOG_RECORDS = CATALOG_ROOT / "output/corpus-performance/model-results.json.gz"
CATALOG_MANIFEST = CATALOG_ROOT / "output/corpus-performance/benchmark-manifest.json"
VECTORIZED_ROOT = CATALOG_ROOT / "output/corpus-performance-vectorized"
GRADIENT_FIELDS = ("stanli_ns_grad", "stanli_ns_grad_mad", "cmdstan_ns_grad",
                   "cmdstan_ns_grad_mad", "paired_speedup", "paired_speedup_mad")
SETUP_PHASES = {"stanli_compile": "stanli-mir", "cmdstan_stanc": "stanc-cpp",
                "cmdstan_build": "cmdstan-build"}


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


def _catalog_config(manifest):
    identity = manifest["identity"]
    config = identity["config"]
    if (not manifest.get("run_id") or identity.get("protocol") != "stanli-corpus-v4"
            or config.get("corpus") != "all" or config.get("filter")
            or type(config.get("rounds")) is not int or config["rounds"] <= 0
            or type(config.get("gradient_budget")) is not int or config["gradient_budget"] != 20000
            or any(key in config for key in ("sampling", "seeds", "iter_sampling", "iter_warmup", "run"))):
        raise ValueError("Catalog requires an unfiltered v4 setup/gradient run with budget 20000")
    for key in ("warmup_ms", "measure_ms"):
        value = config.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError(f"Invalid gradient configuration: {key}")
    return config


def _catalog_pairs(model, record, row, config):
    """Validate the raw paired oracle and recompute every published statistic."""
    rounds = config["rounds"]
    values = {key: _catalog_number(row, key) for key in GRADIENT_FIELDS}
    measured = any(value is not None for value in values.values())
    pairs = record["gradients"]
    if (not isinstance(pairs, list) or len(pairs) > rounds
            or (measured and (len(pairs) != rounds or any(value is None for value in values.values())
                             or _catalog_number(row, "paired_rounds") != rounds))
            or (not measured and (record["status"] == "ok" or row.get("paired_rounds")))):
        raise ValueError(f"Incomplete paired gradient summary: {model}")
    times = {engine: [] for engine in ("stanli", "cmdstan")}
    width = None
    for index, pair in enumerate(pairs):
        order = ["stanli", "cmdstan"] if index % 2 == 0 else ["cmdstan", "stanli"]
        if pair.get("order") != order:
            raise ValueError(f"Gradient order differs: {model}")
        for engine in times:
            trial = pair[engine]
            counters = ("iterations", "elapsed_ns", "warmup_elapsed_ns", "warmup_iterations", "batch")
            vector = trial.get("values")
            if (trial.get("protocol") != "stanli-gradient-v2"
                    or any(type(trial.get(key)) is not int or trial[key] <= 0 for key in counters)
                    or trial["warmup_elapsed_ns"] < config["warmup_ms"] * 1e6
                    or trial["elapsed_ns"] < config["measure_ms"] * 1e6
                    or not isinstance(vector, list) or not vector
                    or any(type(v) not in (int, float) or not math.isfinite(v) for v in vector)):
                raise ValueError(f"Invalid raw gradient trial: {model}")
            if width is not None and len(vector) != width:
                raise ValueError(f"Gradient widths differ: {model}")
            width = len(vector)
            times[engine].append(trial["elapsed_ns"] / trial["iterations"])
        a, b = pair["stanli"]["values"], pair["cmdstan"]["values"]
        worst = max(abs(x-y) / max(abs(x), abs(y), 1) for x, y in zip(a, b))
        if worst > 1e-9 or pair.get("max_scaled_error") != worst:
            raise ValueError(f"Gradient numerical gate differs: {model}")
    if measured:
        if _catalog_number(row, "params") != width - 1:
            raise ValueError(f"Gradient parameter count differs: {model}")
        series = {engine + "_ns_grad": samples for engine, samples in times.items()}
        series["paired_speedup"] = [c/s for s, c in zip(times["stanli"], times["cmdstan"])]
        for field, samples in series.items():
            median = statistics.median(samples)
            mad = statistics.median(abs(value-median) for value in samples)
            _catalog_agrees(model, field, values[field], median)
            _catalog_agrees(model, field + "_mad", values[field + "_mad"], mad)
    return values


def _catalog_setup(model, record, row, config, gradients):
    """Each setup cell requires a successful event; estimates require all terms."""
    setup = record.get("setup", {})
    if not isinstance(setup, dict) or set(setup) - set(SETUP_PHASES):
        raise ValueError(f"Invalid setup evidence: {model}")
    values = {}
    for key, phase in SETUP_PHASES.items():
        event = setup.get(key)
        expected = None
        if event is not None:
            duration = event.get("elapsed_s")
            if (event.get("status") not in ("ok", "failed", "timeout", "interrupted") or event.get("phase") != f"{model}/{phase}"
                    or type(duration) not in (int, float) or not math.isfinite(duration) or duration < 0):
                raise ValueError(f"Invalid setup event: {model} {key}")
            if event["status"] == "ok" and event.get("returncode") != 0:
                raise ValueError(f"Successful setup event lacks exit status zero: {model} {key}")
            expected = duration if event["status"] == "ok" else None
        field = key + "_s"
        values[field] = _catalog_number(row, field)
        _catalog_agrees(model, field, values[field], expected)
    preparations = record.get("preparation_s", [])
    if (not isinstance(preparations, list) or len(preparations) > config["rounds"]
            or any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in preparations)):
        raise ValueError(f"Invalid preparation evidence: {model}")
    values["stanli_prep_s"] = _catalog_number(row, "stanli_prep_s")
    _catalog_agrees(model, "stanli_prep_s", values["stanli_prep_s"],
                    statistics.median(preparations) if len(preparations) == config["rounds"] else None)
    if _catalog_number(row, "gradient_budget") != config["gradient_budget"]:
        raise ValueError(f"Gradient budget differs: {model}")
    for engine, keys in (("stanli", ("stanli_compile_s", "stanli_prep_s")),
                         ("cmdstan", ("cmdstan_stanc_s", "cmdstan_build_s"))):
        components = [values[key] for key in keys] + [gradients[engine + "_ns_grad"]]
        expected = (components[0] + components[1] + config["gradient_budget"] * components[2] / 1e9
                    if all(value is not None for value in components) else None)
        field = engine + "_estimated_s"
        values[field] = _catalog_number(row, field)
        _catalog_agrees(model, field, values[field], expected)
    if record["status"] == "ok" and any(value is None for value in values.values()):
        raise ValueError(f"Successful record lacks setup measurements: {model}")
    return values


def validated_catalog(summary_path, records_path, manifest_path):
    """Return a complete current v4 inventory with validated raw evidence."""
    rows = _catalog_rows(summary_path)
    manifest = json.loads(Path(manifest_path).read_text())
    config = _catalog_config(manifest)
    with gzip.open(records_path, "rt") as stream:
        records = json.load(stream)
    if not isinstance(records, list):
        raise ValueError("Raw model results must be a JSON array")
    details = {record["model"]: record for record in records}
    inputs = manifest["identity"]["inputs"]
    if (not inputs or len(details) != len(records) or set(details) != set(inputs)
            or {row["model"] for row in rows} != set(inputs)):
        raise ValueError("Summary/raw result inventory must match every manifest input")
    for row in rows:
        model = row["model"]
        record = details[model]
        raw = record["row"]
        if row.get("run_id") != manifest["run_id"] or raw.get("run_id") != manifest["run_id"]:
            raise ValueError(f"Gradient run identity differs: {model}")
        if any(not inputs[model].get(key) or record["inputs"].get(key) != inputs[model][key]
               for key in ("stan", "data")):
            raise ValueError(f"Gradient input hashes differ: {model}")
        if any(value != str(raw.get(key, "")) for key, value in row.items()):
            raise ValueError(f"TSV/raw result row differs: {model}")
        if (record.get("status") not in ("ok", "failed", "censored") or "sampling" in record
                or any(key in row for key in ("stanli_sample_s", "cmdstan_sample_s"))):
            raise ValueError(f"Invalid v4 gradient/setup record: {model}")
        gradients = _catalog_pairs(model, record, row, config)
        _catalog_setup(model, record, row, config, gradients)
    return rows, manifest, details


def _catalog_notes(row, record):
    notes = [part.strip() for part in row.get("note", "").split(";") if part.strip()]
    if record["status"] != "ok":
        notes.insert(0, record["status"])
    return "; ".join(notes) or "complete"


def render_catalog(summary_path=CATALOG_SUMMARY, records_path=CATALOG_RECORDS,
                   manifest_path=CATALOG_MANIFEST):
    """One current setup-plus-fixed-gradient proxy table, never sampling time."""
    rows, manifest, details = validated_catalog(summary_path, records_path, manifest_path)
    config = manifest["identity"]["config"]
    output = [
        f"Run `{_catalog_cell(manifest['run_id'])}` ({_catalog_cell(manifest['started_utc'][:10])}): "
        f"{len(rows)} models, {config['rounds']} alternating gradient pairs.", "",
        "Gradient ratio is the median paired CmdStan/Stanli ratio ± MAD. "
        "Estimated seconds are measured setup plus 20,000 × median gradient latency; "
        "this fixed-work proxy is not measured HMC sampling time. Missing components "
        "leave estimates blank; failures remain visible.", "",
        "| Model | Paired gradient ratio ± MAD | Stanli setup + 20,000 gradients (s) | CmdStan equivalent (s) | Notes |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for row in sorted(rows, key=lambda item: item["model"]):
        speedup, mad = (_catalog_number(row, key) for key in ("paired_speedup", "paired_speedup_mad"))
        gradient = f"{speedup:.2f}x ± {mad:.2f}" if speedup is not None else "—"
        estimates = [_catalog_number(row, engine + "_estimated_s") for engine in ("stanli", "cmdstan")]
        cells = [f"`{row['model']}`", gradient,
                 *(f"{value:.4g}" if value is not None else "—" for value in estimates),
                 _catalog_notes(row, details[row["model"]])]
        output.append("| " + " | ".join(_catalog_cell(cell) for cell in cells) + " |")
    return "\n".join(output) + "\n"


def render_gradient_catalog(summary_path=VECTORIZED_ROOT / "benchmark-summary.tsv",
                            manifest_path=VECTORIZED_ROOT / "benchmark-manifest.json",
                            records_path=VECTORIZED_ROOT / "model-results.json.gz",
                            provenance_path=VECTORIZED_ROOT / "compiler-provenance.json",
                            default_manifest_path=CATALOG_MANIFEST):
    """Current O1+vectorization gradients with the same runtime and inputs."""
    rows, manifest, details = validated_catalog(summary_path, records_path, manifest_path)
    baseline = json.loads(Path(default_manifest_path).read_text())
    default_config = _catalog_config(baseline)
    provenance = json.loads(Path(provenance_path).read_text())
    identity, default = manifest["identity"], baseline["identity"]
    config = identity["config"]
    if "--O1" not in shlex.split(config.get("stancflags", "")):
        raise ValueError("Vectorized catalog requires O1 compiler flags")
    if identity["inputs"] != default["inputs"]:
        raise ValueError("Vectorized/default input inventories differ")
    for name in ("bench", "vectorize_probe"):
        digest = identity["executables"].get(name)
        if not digest or digest != default["executables"].get(name):
            raise ValueError(f"Vectorized/default executable hashes differ: {name}")
    for key in ("rounds", "warmup_ms", "measure_ms", "gradient_timeout", "gradient_budget"):
        if config.get(key) != default_config.get(key):
            raise ValueError(f"Vectorized/default gradient configuration differs: {key}")
    if identity.get("threads") != default.get("threads"):
        raise ValueError("Vectorized/default thread settings differ")
    if (provenance.get("optimization") != "O1 plus vectorize_loops"
            or provenance.get("binary_sha256") != identity["executables"].get("stanc")
            or not re.fullmatch(r"[0-9a-f]{40}", provenance.get("source_sha", ""))
            or any(not re.fullmatch(r"[0-9a-f]{64}", provenance.get(key, ""))
                   for key in ("binary_sha256", "patch_sha256"))
            or not isinstance(provenance.get("build_commands"), list) or not provenance["build_commands"]
            or not all(isinstance(command, str) and command.strip() for command in provenance["build_commands"])):
        raise ValueError("Invalid optimized compiler provenance")
    output = [
        f"Run `{_catalog_cell(manifest['run_id'])}`: {len(rows)} models, {config['rounds']} alternating pairs. "
        "Gradient latencies are microseconds (median ± MAD); speedup is the median "
        "within-pair CmdStan/Stanli ratio ± MAD. Missing measurements are —.", "",
        "| Model | Stanli µs | CmdStan O1+vec µs | Paired speedup ± MAD | Notes |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for row in sorted(rows, key=lambda item: item["model"]):
        speedup = _catalog_number(row, "paired_speedup")
        if speedup is not None:
            cells = [f"{_catalog_number(row, engine + '_ns_grad')/1000:.4g} ± "
                     f"{_catalog_number(row, engine + '_ns_grad_mad')/1000:.3g}" for engine in ("stanli", "cmdstan")]
            speed = f"{speedup:.2f}x ± {_catalog_number(row, 'paired_speedup_mad'):.2f}"
        else:
            cells, speed = ["—", "—"], "—"
        cells = [f"`{row['model']}`", *cells, speed, _catalog_notes(row, details[row["model"]])]
        output.append("| " + " | ".join(_catalog_cell(cell) for cell in cells) + " |")
    return "\n".join(output) + "\n"


def require_historical_tsv(col):
    if "gradient_budget" in col.columns:
        raise SystemExit("Current v4 TSVs require raw evidence validation: publish with "
                         "tools/publish_corpus_bench.py, then use tools/corpus_table.py --catalog "
                         "(or --vectorized-catalog). Plain TSV mode is historical only.")


def main():
    if "--vectorized-catalog" in sys.argv:
        print(render_gradient_catalog(), end="")
        return
    if "--catalog" in sys.argv:
        print(render_catalog(), end="")
        return
    if "--gradients" in sys.argv:
        path = sys.argv[sys.argv.index("--gradients") + 1]
        rows, col = load_rows(path)
        require_historical_tsv(col)
        render_gradients(rows, col)
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
    require_historical_tsv(col)
    if o1vec:
        render_o1vec(rows, col)
    else:
        render_main(rows, col)


if __name__ == "__main__":
    main()
