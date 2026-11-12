#!/usr/bin/env python3
"""Render the benchmark catalog for benchmarks.md.

--catalog renders one complete, current benchmark run for benchmarks.md.
It requires the current full-corpus v5 or v6 summary, raw model records and
manifest under output/corpus-performance; it never substitutes historical measurements.
Its total-sampling estimate is setup plus a gradient count times the mean gradient time,
not measured sampling. A v6 run takes each model's count from a Stanli reference run
(1,000 warmup and 1,000 draws) and times the gradient at ten points of that run; a model
whose reference run failed uses the fixed points and 5,000 gradients. A v5 run uses the
fixed points and 5,000 gradients for every model. Gradient times are means over the
recorded evaluation points. The records keep per-point timings and scaled errors, not
density or gradient values; the run enforced the gate.

--vectorized-catalog renders the loop-vectorization comparison.
"""
import csv
import gzip
import html
import shlex
import re
import json
import math
import statistics
from pathlib import Path
import sys

CATALOG_ROOT = Path(__file__).resolve().parents[1]
CATALOG_SUMMARY = CATALOG_ROOT / "output/corpus-performance/benchmark-summary.tsv"
CATALOG_RECORDS = CATALOG_ROOT / "output/corpus-performance/model-results.json.gz"
CATALOG_MANIFEST = CATALOG_ROOT / "output/corpus-performance/benchmark-manifest.json"
VECTORIZED_ROOT = CATALOG_ROOT / "output/corpus-performance-vectorized"
FIXED_GRADIENT_COUNT = 5000
LEGACY_GRADIENT_BUDGET = 20000
GRADIENT_FIELDS = ("stanli_ns_grad", "stanli_ns_grad_mad", "cmdstan_ns_grad",
                   "cmdstan_ns_grad_mad", "paired_speedup", "paired_speedup_mad")
CACHE_FIELDS = ("cached", "cache_key", "cache_run_id")
PROTOCOLS = ("stanli-corpus-v5", "stanli-corpus-v6")
POINT_SOURCES = ("fixed", "reference_run")
FALLBACK_MARK = "†"
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


def _is_digest(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _point_source(model, protocol, manifest_input, record, row):
    """The recorded gradient count, which the manifest, raw record and summary row must agree on."""
    keys = ("point_source", "points_sha256", "gradient_count", "reference_sha256")
    if protocol == "stanli-corpus-v5":
        if any(key in source for source in (manifest_input, record["inputs"], row) for key in keys):
            raise ValueError(f"v5 runs have no point source: {model}")
        return FIXED_GRADIENT_COUNT
    source, count = manifest_input.get("point_source"), manifest_input.get("gradient_count")
    inputs = record["inputs"]
    if (source not in POINT_SOURCES or type(count) is not int or count < 1
            or (source == "fixed" and count != FIXED_GRADIENT_COUNT)
            or not _is_digest(manifest_input.get("reference_sha256"))
            or (source == "reference_run") != ("points_sha256" in manifest_input)
            or ("points_sha256" in manifest_input and not _is_digest(manifest_input["points_sha256"]))
            or row.get("point_source") != source or row.get("gradient_count") != str(count)
            or any(inputs.get(key) != manifest_input.get(key)
                   for key in ("point_source", "gradient_count", "reference_sha256", "points_sha256"))):
        raise ValueError(f"Invalid or inconsistent point source: {model}")
    return count


def _catalog_cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def _catalog_config(manifest, allow_filter=False):
    identity = manifest["identity"]
    config = identity["config"]
    legacy = identity.get("protocol") == "stanli-corpus-v5"
    if (not manifest.get("run_id")
            or identity.get("protocol") not in PROTOCOLS
            or config.get("corpus") != "all" or (config.get("filter") and not allow_filter)
            or type(config.get("rounds")) is not int or config["rounds"] <= 0
            or ("gradient_budget" in config) != legacy
            or (legacy and (type(config["gradient_budget"]) is not int
                            or config["gradient_budget"] != LEGACY_GRADIENT_BUDGET))
            or any(key in config for key in ("sampling", "seeds", "iter_sampling", "iter_warmup", "run"))):
        raise ValueError("Catalog requires an unfiltered v5 or v6 setup/gradient run")
    for key in ("warmup_ms", "measure_ms"):
        value = config.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError(f"Invalid gradient configuration: {key}")
    points = config.get("points")
    if type(points) is not int or not 1 <= points <= 100:
        raise ValueError("Invalid gradient configuration: points")
    return config


def _trial_points(model, trial, config):
    """Validate one raw driver output and return its per-point observations."""
    entries = trial.get("points")
    if not (trial.get("protocol") == "stanli-gradient-v3"
            and all(type(trial.get(key)) is int and trial[key] > 0
                    for key in ("warmup_iterations", "warmup_elapsed_ns"))
            and trial["warmup_elapsed_ns"] >= config["warmup_ms"] * 1e6
            and isinstance(entries, list) and len(entries) == config["points"]):
        raise ValueError(f"Invalid raw gradient trial: {model}")
    for index, entry in enumerate(entries):
        if entry.get("point") != index or type(entry.get("finite")) is not bool:
            raise ValueError(f"Invalid raw gradient trial: {model}")
        if not entry["finite"]:
            if set(entry) != {"point", "finite"}:
                raise ValueError(f"Invalid raw gradient trial: {model}")
            continue
        if (set(entry) != {"point", "finite", "iterations", "elapsed_ns", "batch"}
                or any(type(entry[key]) is not int or entry[key] <= 0
                       for key in ("iterations", "elapsed_ns", "batch"))
                or entry["elapsed_ns"] < config["measure_ms"] * 1e6):
            raise ValueError(f"Invalid raw gradient trial: {model}")
    if not any(entry["finite"] for entry in entries):
        raise ValueError(f"Invalid raw gradient trial: {model}")
    return entries


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
    width = kept_first = dropped_first = None
    for index, pair in enumerate(pairs):
        order = ["stanli", "cmdstan"] if index % 2 == 0 else ["cmdstan", "stanli"]
        if pair.get("order") != order:
            raise ValueError(f"Gradient order differs: {model}")
        points = {engine: _trial_points(model, pair[engine], config) for engine in times}
        kept, dropped = [], []
        for a, b in zip(points["stanli"], points["cmdstan"]):
            if a["finite"] != b["finite"]:
                raise ValueError(f"Gradient numerical gate differs: {model}")
            (kept if a["finite"] else dropped).append(a["point"])
        errors = pair.get("scaled_errors")
        if (pair.get("kept_points") != kept or pair.get("dropped_points") != dropped
                or not kept or not isinstance(errors, list) or len(errors) != len(kept)
                or any(type(e) not in (int, float) or not 0 <= e <= 1e-9 for e in errors)
                or pair.get("max_scaled_error") != max(errors)):
            raise ValueError(f"Gradient numerical gate differs: {model}")
        if type(pair.get("width")) is not int or pair["width"] <= 0 or (
                width is not None and pair["width"] != width):
            raise ValueError(f"Gradient widths differ: {model}")
        width = pair["width"]
        if kept_first is not None and kept != kept_first:
            raise ValueError(f"Gradient points differ between rounds: {model}")
        kept_first, dropped_first = kept, dropped
        for engine in times:
            times[engine].append(statistics.fmean(
                points[engine][k]["elapsed_ns"] / points[engine][k]["iterations"] for k in kept))
    if measured:
        if _catalog_number(row, "params") != width - 1:
            raise ValueError(f"Gradient parameter count differs: {model}")
        _catalog_agrees(model, "points_kept", _catalog_number(row, "points_kept"), len(kept_first))
        _catalog_agrees(model, "points_dropped", _catalog_number(row, "points_dropped"),
                        len(dropped_first))
        series = {engine + "_ns_grad": samples for engine, samples in times.items()}
        series["paired_speedup"] = [c/s for s, c in zip(times["stanli"], times["cmdstan"])]
        for field, samples in series.items():
            median = statistics.median(samples)
            mad = statistics.median(abs(value-median) for value in samples)
            _catalog_agrees(model, field, values[field], median)
            _catalog_agrees(model, field + "_mad", values[field + "_mad"], mad)
    return values


def _catalog_setup(model, record, row, config, gradients, count):
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
            if any(field in event for field in CACHE_FIELDS) and (
                    key == "stanli_compile" or event["status"] != "ok" or event.get("cached") is not True
                    or any(not isinstance(event.get(field), str) or not event[field]
                           for field in CACHE_FIELDS[1:])):
                raise ValueError(f"Invalid setup event: {model} {key}")
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
    legacy = "gradient_budget" in config
    if legacy and _catalog_number(row, "gradient_budget") != config["gradient_budget"]:
        raise ValueError(f"Gradient budget differs: {model}")
    for engine, keys in (("stanli", ("stanli_compile_s", "stanli_prep_s")),
                         ("cmdstan", ("cmdstan_stanc_s", "cmdstan_build_s"))):
        components = [values[key] for key in keys] + [gradients[engine + "_ns_grad"]]
        expected = (components[0] + components[1] + (config["gradient_budget"] if legacy else count) * components[2] / 1e9
                    if all(value is not None for value in components) else None)
        field = engine + "_estimated_s"
        values[field] = _catalog_number(row, field)
        _catalog_agrees(model, field, values[field], expected)
    if record["status"] == "ok" and any(value is None for value in values.values()):
        raise ValueError(f"Successful record lacks setup measurements: {model}")
    return values


def validated_catalog(summary_path, records_path, manifest_path, allow_filter=False):
    """Return a complete current v5 inventory with validated raw evidence."""
    rows = _catalog_rows(summary_path)
    manifest = json.loads(Path(manifest_path).read_text())
    config = _catalog_config(manifest, allow_filter)
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
            raise ValueError(f"Invalid gradient/setup record: {model}")
        count = _point_source(model, manifest["identity"]["protocol"], inputs[model], record, row)
        gradients = _catalog_pairs(model, record, row, config)
        _catalog_setup(model, record, row, config, gradients, count)
    return rows, manifest, details


def _catalog_notes(row, record):
    notes = [part.strip() for part in row.get("note", "").split(";") if part.strip()]
    if record["status"] != "ok":
        notes.insert(0, record["status"])
    first = record["gradients"][0] if record.get("gradients") else {}
    if first.get("dropped_points"):
        total = len(first["kept_points"]) + len(first["dropped_points"])
        notes.append(f"{len(first['dropped_points'])} of {total} points non-finite in both engines; dropped")
    return "; ".join(notes) or "complete"


def _catalog_model(model):
    # GitHub strips <wbr> and hides <br> inside code spans, so break between spans.
    if len(model) <= 24:
        return f"`{model}`"
    lines = [""]
    for part in re.split(r"(?<=_)", model):
        if lines[-1] and len(lines[-1]) + len(part) > 24:
            lines.append("")
        lines[-1] += part
    return "<br>".join(f"<code>{html.escape(line)}</code>" for line in lines)


def _count(n):
    return f"{n / 1e6:.3g}M" if n >= 1e6 else f"{n / 1e3:.3g}k"


def _catalog_parts(rows, details, speedup_field="paired_speedup"):
    """Complete-row lines, incomplete-row lines and the incomplete rows' reasons."""
    complete_lines, incomplete_lines, reasons = [], [], []
    complete = sorted(
        (row for row in rows if details[row["model"]]["status"] == "ok"),
        key=lambda row: (-_catalog_number(row, speedup_field), row["model"]),
    )
    incomplete_rows = sorted(
        (row for row in rows if details[row["model"]]["status"] != "ok"),
        key=lambda row: row["model"],
    )
    for row in (*complete, *incomplete_rows):
        speedup = _catalog_number(row, speedup_field)
        name = _catalog_model(row["model"])
        if row.get("point_source") == "fixed":
            name += f" {FALLBACK_MARK}"
        count = int(row.get("gradient_count") or FIXED_GRADIENT_COUNT)
        cells = [name, f"{speedup:.3g}x" if speedup is not None else "—", _count(count)]
        estimates = []
        for engine, fields in (("stanli", ("stanli_compile_s", "stanli_prep_s")),
                               ("cmdstan", ("cmdstan_stanc_s", "cmdstan_build_s"))):
            components = [_catalog_number(row, field) for field in (*fields, engine + "_ns_grad")]
            estimate = (components[0] + components[1] + count * components[2] / 1e9
                        if all(value is not None for value in components) else None)
            estimates.append(estimate)
            cells.append(f"{estimate:.3g}" if estimate is not None else "—")
        if cells[3:] == ["—", "—"]:
            cells[2] = "—"
        stanli_total, cmdstan_total = estimates
        cells.append(f"{cmdstan_total / stanli_total:.3g}x"
                     if stanli_total and cmdstan_total is not None else "—")
        record = details[row["model"]]
        if record["status"] == "ok":
            complete_lines.append("| " + " | ".join(_catalog_cell(cell) for cell in cells) + " |")
        else:
            note = _catalog_notes(row, record)
            reasons.append((row["model"], note))
            cells.append(note)
            incomplete_lines.append("| " + " | ".join(_catalog_cell(cell) for cell in cells) + " |")
    return complete_lines, incomplete_lines, reasons


CATALOG_HEADER = ("| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) "
                  "| CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |")


def _catalog_tables(rows, details, points, speedup_field="paired_speedup"):
    """Present both compiler configurations with the same public workload."""
    complete, incomplete, _ = _catalog_parts(rows, details, speedup_field)
    output = [CATALOG_HEADER, "| --- | ---: | ---: | ---: | ---: | ---: |", *complete]
    referenced = any(row.get("point_source") == "reference_run" for row in rows)
    if referenced:
        output.extend([
            "", "Total sampling is an estimate: each engine's measured setup time + the number of gradients "
            "a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, "
            f"averaged over {points} points of that run. The same gradient count is used for both engines. "
            "Full sampling is not run."])
        if any(row.get("point_source") == "fixed" for row in rows):
            output.extend(["", f"{FALLBACK_MARK} The reference run did not finish, so this model uses "
                               f"{FIXED_GRADIENT_COUNT:,} gradients at the fixed points."])
    else:
        output.extend([
            "", f"Total sampling is an estimate: each engine's measured setup time + {FIXED_GRADIENT_COUNT:,} × "
            f"its warm gradient time, averaged over {points} evaluation points. Full sampling is not run."])
    if incomplete:
        output.extend(["", "**Incomplete results**", "",
                       "Missing measurements are —; available timings are retained.", "",
                       CATALOG_HEADER + " Reason |", "| --- | ---: | ---: | ---: | ---: | ---: | --- |", *incomplete])
    return "\n".join(output) + "\n"


def _failure_lists(reasons):
    """Lists of the models left out of every speedup, grouped by why."""
    numerical = [(model, note) for model, note in reasons if "scaled error" in note]
    other = [(model, note) for model, note in reasons if "scaled error" not in note]
    blocks = []
    if numerical:
        blocks.append("### Fails the numerical check\n\n"
                      "These models are left out of every speedup on this page. "
                      "The gate is 1e-9 scaled error ([details](#numerical-gate)).\n\n"
                      + "\n".join(f"- `{model}`: {note.removeprefix('failed; ')}"
                                  for model, note in numerical))
    if other:
        blocks.append("### Not measured\n\n"
                      + "\n".join(f"- `{model}`: {note}" for model, note in other))
    return "\n\n".join(blocks) or "### Fails the numerical check\n\nNo model fails."


def _cached_models(details):
    return sum(1 for record in details.values()
               if any(event.get("cached") for event in record.get("setup", {}).values()))


def _reuse_note(count, total, where="this run"):
    if not count:
        return ""
    return (f"CmdStan stanc and build times for {count} of {total} models were reused from "
            f"the build cache and not remeasured in {where}.\n\n")


def render_catalog(summary_path=CATALOG_SUMMARY, records_path=CATALOG_RECORDS,
                   manifest_path=CATALOG_MANIFEST, allow_filter=False):
    """One current setup-plus-fixed-gradient proxy table, never sampling time."""
    rows, manifest, details = validated_catalog(summary_path, records_path, manifest_path,
                                                allow_filter)
    config = manifest["identity"]["config"]
    intro = (
        f"Run `{_catalog_cell(manifest['run_id'])}` ({_catalog_cell(manifest['started_utc'][:10])}): "
        f"{len(rows)} models, {config['rounds']} alternating gradient pairs.\n\n"
    )
    return (intro + _reuse_note(_cached_models(details), len(rows))
            + _catalog_tables(rows, details, config["points"]))


def render_catalog_sections(summary_path=CATALOG_SUMMARY, records_path=CATALOG_RECORDS,
                            manifest_path=CATALOG_MANIFEST, allow_filter=False):
    """The pieces of the full-corpus section of docs/benchmarks.md, one string each."""
    rows, manifest, details = validated_catalog(summary_path, records_path, manifest_path,
                                                allow_filter)
    complete, _, reasons = _catalog_parts(rows, details)
    notes = [f"Run `{_catalog_cell(manifest['run_id'])}` ({_catalog_cell(manifest['started_utc'][:10])})."]
    if any(row.get("point_source") == "fixed" for row in rows):
        notes.append(f"{FALLBACK_MARK} The reference run did not finish, so this model uses "
                     f"{FIXED_GRADIENT_COUNT:,} gradients at the fixed points.")
    reused = _cached_models(details)
    runs = (f"CmdStan stanc and build times for {reused} of {len(rows)} models were reused from "
            "the build cache and not remeasured." if reused
            else "Every CmdStan stanc and build time was measured in this run.")
    return {
        "table": "\n".join([CATALOG_HEADER, "| --- | ---: | ---: | ---: | ---: | ---: |", *complete]),
        "notes": "\n\n".join(notes),
        "runs": runs,
        "failures": _failure_lists(reasons),
    }


DEFAULT_ROOT = CATALOG_ROOT / "output/corpus-performance"


def _run_artifacts(directory, allow_filter=False):
    directory = Path(directory)
    return validated_catalog(directory / "benchmark-summary.tsv", directory / "model-results.json.gz",
                             directory / "benchmark-manifest.json", allow_filter)


def _cmdstan_toolchain(identity):
    return {key: value for key, value in (identity.get("toolchains") or {}).items()
            if not key.startswith("stanli_")}


REUSE_SOURCES = ("tools/bench_cmdstan_grad.cpp", "tools/benchmark_timer.hpp")


def _machine(identity):
    machine = identity.get("machine")
    return {key: value for key, value in machine.items() if key != "host"} if machine else None


def _check_optimized_run(manifest, default, provenance):
    identity, baseline = manifest["identity"], default["identity"]
    config, default_config = identity["config"], _catalog_config(default)
    if "--O1" not in shlex.split(config.get("stancflags", "")):
        raise ValueError("Vectorized catalog requires O1 compiler flags")
    for key in ("rounds", "warmup_ms", "measure_ms", "points", "gradient_timeout", "gradient_budget"):
        if config.get(key) != default_config.get(key):
            raise ValueError(f"Vectorized/default gradient configuration differs: {key}")
    if identity.get("threads") != baseline.get("threads"):
        raise ValueError("Vectorized/default thread settings differ")
    if _cmdstan_toolchain(identity) != _cmdstan_toolchain(baseline):
        raise ValueError("Vectorized/default CmdStan toolchain differs")
    if any(not identity.get("sources", {}).get(path)
           or identity["sources"][path] != baseline.get("sources", {}).get(path)
           for path in REUSE_SOURCES):
        raise ValueError("Vectorized/default CmdStan driver sources differ")
    if _machine(identity) is None or _machine(identity) != _machine(baseline):
        raise ValueError("Vectorized/default machine differs")
    if (provenance.get("optimization") != "O1 plus vectorize_loops"
            or provenance.get("binary_sha256") != identity["executables"].get("stanc")
            or not re.fullmatch(r"[0-9a-f]{40}", provenance.get("source_sha", ""))
            or any(not re.fullmatch(r"[0-9a-f]{64}", provenance.get(key, ""))
                   for key in ("binary_sha256", "patch_sha256"))
            or not isinstance(provenance.get("build_commands"), list) or not provenance["build_commands"]
            or not all(isinstance(command, str) and command.strip() for command in provenance["build_commands"])):
        raise ValueError("Invalid optimized compiler provenance")


def combined_vectorized_rows(root=VECTORIZED_ROOT, default_root=DEFAULT_ROOT):
    """Default-run Stanli timings against vectorized CmdStan timings from earlier runs."""
    root = Path(root)
    default_rows, default, default_details = _run_artifacts(default_root)
    combined = json.loads((root / "combined.json").read_text())
    provenance = json.loads((root / "compiler-provenance.json").read_text())
    if combined.get("stanli_run_id") != default["run_id"]:
        raise ValueError("Stanli run differs from the default run")
    sources = combined.get("cmdstan_runs", {})
    inputs = default["identity"]["inputs"]
    if set(sources) != set(inputs):
        raise ValueError("Vectorized CmdStan inventory must cover every default input")
    runs = {}
    for run_id in sorted(set(sources.values())):
        rows, manifest, details = _run_artifacts(root / "cmdstan-runs" / run_id, allow_filter=True)
        if manifest["run_id"] != run_id:
            raise ValueError(f"CmdStan run identity differs: {run_id}")
        _check_optimized_run(manifest, default, provenance)
        runs[run_id] = ({row["model"]: row for row in rows}, manifest, details)
    output, statuses = [], {}
    for row in default_rows:
        model = row["model"]
        cmdstan_rows, manifest, details = runs[sources[model]]
        if model not in details or manifest["identity"]["inputs"].get(model) != inputs[model]:
            raise ValueError(f"Vectorized/default input hashes differ: {model}")
        source, stanli = cmdstan_rows[model], default_details[model]
        status, note = "ok", ""
        if stanli["status"] != "ok":
            status, note = stanli["status"], row.get("note", "")
        elif details[model]["status"] != "ok":
            status, note = details[model]["status"], source.get("note", "")
        merged = dict(model=model, run_id=sources[model], note=note,
                      **{key: row.get(key, "") for key in ("stanli_compile_s", "stanli_prep_s",
                                                           "stanli_ns_grad", "point_source", "gradient_count")},
                      **{key: source.get(key, "") for key in ("cmdstan_stanc_s", "cmdstan_build_s",
                                                              "cmdstan_ns_grad")})
        if status == "ok":
            merged["median_ratio"] = (_catalog_number(merged, "cmdstan_ns_grad")
                                      / _catalog_number(merged, "stanli_ns_grad"))
        else:
            merged["stanli_ns_grad"] = merged["cmdstan_ns_grad"] = ""
        output.append(merged)
        statuses[model] = {"status": status, "cached": bool(_cached_models({model: details[model]}))}
    return output, statuses, default, {run_id: manifest for run_id, (_, manifest, _) in runs.items()}


def _vectorized_sources(rows, manifests):
    counts = {}
    for row in rows:
        counts[row["run_id"]] = counts.get(row["run_id"], 0) + 1
    main, *extra = sorted(manifests, key=lambda run_id: -counts.get(run_id, 0))
    describe = lambda run_id: (f"run `{_catalog_cell(run_id)}` "
                               f"({_catalog_cell(manifests[run_id]['started_utc'][:10])})")
    supplements = "".join(
        " " + ", ".join(f"`{row['model']}`" for row in rows if row["run_id"] == run_id)
        + f" is from {describe(run_id)}." for run_id in extra)
    return f"The CmdStan timings with loop vectorization are reused from {describe(main)}.{supplements}"


def _vectorized_intro(rows, manifests, default):
    return (f"Stanli timings are from the default run `{_catalog_cell(default['run_id'])}`. "
            f"{_vectorized_sources(rows, manifests)} Each ratio is CmdStan's median gradient time "
            f"divided by Stanli's, each averaged over {default['identity']['config']['points']} "
            "evaluation points.\n\n")


def render_gradient_catalog(root=VECTORIZED_ROOT, default_root=DEFAULT_ROOT):
    """O1+vectorization CmdStan timings reused from earlier runs against the default Stanli run."""
    rows, statuses, default, manifests = combined_vectorized_rows(root, default_root)
    intro = _vectorized_intro(rows, manifests, default)
    reused = _reuse_note(sum(status["cached"] for status in statuses.values()), len(rows),
                         "the runs that report them")
    return intro + reused + _catalog_tables(rows, statuses, default["identity"]["config"]["points"],
                                            "median_ratio")


VECTORIZED_TOP = 10
VECTORIZED_CHANGE = 0.10


def render_vectorized_sections(root=VECTORIZED_ROOT, default_root=DEFAULT_ROOT):
    """Summary of the loop-vectorization comparison; the full table is the evidence README."""
    rows, statuses, default, manifests = combined_vectorized_rows(root, default_root)
    default_rows, _, _ = _run_artifacts(default_root)
    baseline = {row["model"]: _catalog_number(row, "paired_speedup") for row in default_rows}
    pairs = [(row["model"], baseline[row["model"]], row["median_ratio"])
             for row in rows if statuses[row["model"]]["status"] == "ok"
             and baseline.get(row["model"]) and row["median_ratio"]]
    default_median = statistics.median(value for _, value, _ in pairs)
    vectorized_median = statistics.median(value for _, _, value in pairs)
    limit = 1 + VECTORIZED_CHANGE
    smaller = sum(vectorized * limit < value for _, value, vectorized in pairs)
    larger = sum(vectorized > value * limit for _, value, vectorized in pairs)
    top = sorted(pairs, key=lambda pair: (-max(pair[1] / pair[2], pair[2] / pair[1]), pair[0]))
    table = ["| Model | Default CmdStan | Vectorized CmdStan |", "| --- | ---: | ---: |"]
    table += [f"| `{model}` | {value:.3g}x | {vectorized:.3g}x |"
              for model, value, vectorized in top[:VECTORIZED_TOP]]
    summary = (
        f"With loop vectorization on, the median model's gradient is {vectorized_median:.2f}x faster in "
        f"Stanli than in CmdStan, against {default_median:.2f}x for default CmdStan. The speedup changes by "
        f"more than {VECTORIZED_CHANGE:.0%} on {smaller + larger} of {len(pairs)} models: it shrinks on "
        f"{smaller} and grows on {larger}. These {VECTORIZED_TOP} models change most, in how many times "
        "faster Stanli's gradient is:\n\n" + "\n".join(table))
    runs = "Stanli timings in the loop-vectorization comparison are from the default run. " + _vectorized_sources(rows, manifests)
    return {"summary": summary, "runs": runs}


def main():
    if "--vectorized-catalog" in sys.argv:
        print(render_gradient_catalog(), end="")
        return
    if "--catalog" in sys.argv:
        print(render_catalog(), end="")
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
