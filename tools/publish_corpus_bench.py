#!/usr/bin/env python3
"""Publish a complete v5 or v6 corpus run without rerunning measurements.

python3 tools/publish_corpus_bench.py RUN.tsv OUTPUT_DIRECTORY
python3 tools/publish_corpus_bench.py RUN.tsv OUTPUT_DIRECTORY --rerun RERUN.tsv... [--max-load 6]
python3 tools/publish_corpus_bench.py --vectorized OUTPUT_DIRECTORY \
    --stanli-run output/corpus-performance --cmdstan-run CMDSTAN_RUN_DIRECTORY...
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from corpus_table import render_catalog, render_gradient_catalog
from loaded_models import DEFAULT_MAX_LOAD, plan_substitutions


def _summary_lines(summary):
    header, *lines = Path(summary).read_text().splitlines(keepends=True)
    return header, {line.split("\t", 1)[0]: line for line in lines}


def _substituted(summary, records, manifest, reruns, max_load):
    """The primary run's summary and records with each loaded model taken from its rerun."""
    run = Path(str(summary) + ".run")
    loaded = [(json.loads((Path(str(rerun) + ".run") / "manifest.json").read_text()),
               (Path(str(rerun) + ".run") / "events.jsonl").read_text()) for rerun in reruns]
    for rerun, (rerun_manifest, _) in zip(reruns, loaded):
        if len(list(Path(str(rerun) + ".run").glob("*.result.json"))) != len(rerun_manifest["identity"]["inputs"]):
            raise ValueError(f"Cannot publish an incomplete rerun: {rerun}")
    suppliers, still_loaded = plan_substitutions(manifest, (run / "events.jsonl").read_text(),
                                                 loaded, max_load)
    header, lines = _summary_lines(summary)
    records = {record["model"]: record for record in records}
    for rerun, (rerun_manifest, _) in zip(reruns, loaded):
        rerun_header, rerun_lines = _summary_lines(rerun)
        if rerun_header != header:
            raise ValueError(f"Rerun summary columns differ: {rerun}")
        for model in rerun_manifest["identity"]["inputs"]:
            if suppliers[model] == rerun_manifest["run_id"]:
                lines[model] = rerun_lines[model]
                records[model] = json.loads(
                    (Path(str(rerun) + ".run") / f"{model}.result.json").read_text())
    substitutions = dict(primary_run_id=manifest["run_id"], max_load=max_load,
                         reruns=[rerun["run_id"] for rerun, _ in loaded],
                         models={m: r for m, r in suppliers.items() if r != manifest["run_id"]},
                         still_loaded=still_loaded)
    return (header + "".join(lines[model] for model in sorted(lines)),
            [records[model] for model in sorted(records)], substitutions)


def publish(summary, output, compiler_evidence=None, build_identity=None, reruns=(),
            max_load=DEFAULT_MAX_LOAD):
    run = Path(str(summary) + ".run")
    manifest = json.loads((run / "manifest.json").read_text())
    records = [json.loads(path.read_text())
               for path in sorted(run.glob("*.result.json"))]
    if len(records) != len(manifest["identity"]["inputs"]):
        raise ValueError("Cannot publish an incomplete corpus run")
    if output.exists():
        raise ValueError(f"Output already exists: {output}")
    substitutions = None
    if reruns:
        text, records, substitutions = _substituted(summary, records, manifest, reruns, max_load)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".corpus-publish-", dir=output.parent) as temporary:
        staging = Path(temporary) / "evidence"
        staging.mkdir()
        if substitutions:
            (staging / "benchmark-summary.tsv").write_text(text)
            (staging / "substitutions.json").write_text(json.dumps(substitutions, indent=2) + "\n")
            # Each rerun keeps its own manifest and events so the load rule can be checked again.
            for rerun, run_id in zip(reruns, substitutions["reruns"]):
                publish(rerun, staging / "reruns" / run_id)
        else:
            shutil.copyfile(summary, staging / "benchmark-summary.tsv")
        shutil.copyfile(run / "manifest.json", staging / "benchmark-manifest.json")
        for name, content in (("model-results.json.gz", json.dumps(records, allow_nan=False).encode()),
                              ("events.jsonl.gz", (run / "events.jsonl").read_bytes())):
            (staging / name).write_bytes(gzip.compress(content, mtime=0))
        table = render_catalog(staging / "benchmark-summary.tsv",
                               staging / "model-results.json.gz",
                               staging / "benchmark-manifest.json", allow_filter=True)
        if compiler_evidence:
            for name in ("compiler-provenance.json", "stanc-o1vec.patch",
                         "stock-sentinel.mir", "vectorized-sentinel.mir",
                         "vectorization-sentinel.stan"):
                shutil.copyfile(compiler_evidence / name, staging / name)
        if build_identity:
            shutil.copyfile(build_identity, staging / "build-identity.json")
        (staging / "README.md").write_text(
            "# Corpus benchmark evidence\n\n"
            f"Run `{manifest['run_id']}`, started {manifest['started_utc']}, "
            f"records {len(records)} application models.\n\n"
            "The manifest identifies the exact sources, inputs, binaries and settings. "
            "The summary, raw paired observations and command events are retained here. "
            "The table below gives each model's setup time plus its gradient count times its "
            "measured gradient time. Full sampling is not run. Failures remain in the inventory.\n\n"
            f"Full inputs, generated code and command logs remain locally at `{run}`; "
            "these larger artifacts are not hosted with this report.\n\n" + table)
        _checksums(staging)
        staging.rename(output)
    print(f"Published {len(records)} outcomes from {manifest['run_id']} to {output}")


def _checksums(directory):
    checksums = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}"
                 for path in sorted(directory.iterdir())
                 if path.is_file() and path.name != "SHA256SUMS"]
    (directory / "SHA256SUMS").write_text("\n".join(checksums) + "\n")


def publish_vectorized(output, stanli_run, cmdstan_runs):
    """Pair the default run's Stanli timings with already published vectorized CmdStan runs."""
    output = Path(output)
    default = json.loads((Path(stanli_run) / "benchmark-manifest.json").read_text())
    sources = {}
    for run in cmdstan_runs:
        manifest = json.loads((Path(run) / "benchmark-manifest.json").read_text())
        if Path(run).resolve().parent != (output / "cmdstan-runs").resolve() or Path(run).name != manifest["run_id"]:
            raise ValueError(f"CmdStan runs must be published under {output / 'cmdstan-runs'}/RUN_ID")
        for model in manifest["identity"]["inputs"]:
            sources[model] = manifest["run_id"]
    combined = {"stanli_run_id": default["run_id"],
                "cmdstan_runs": {model: sources[model] for model in sorted(default["identity"]["inputs"])
                                 if model in sources}}
    (output / "combined.json").write_text(json.dumps(combined, indent=2) + "\n")
    table = render_gradient_catalog(output, stanli_run)
    (output / "README.md").write_text(
        "# Vectorized CmdStan comparison\n\n"
        "`combined.json` names the default Stanli run and, for each model, the published "
        "vectorized CmdStan run under `cmdstan-runs/` whose timings are used. Each run "
        "directory keeps that run's manifest, summary, raw observations and events. "
        "The table uses each model's gradient count from the default run. Full sampling is not run.\n\n" + table)
    _checksums(output)
    print(f"Published {len(combined['cmdstan_runs'])} vectorized comparisons to {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path, nargs="?")
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--compiler-evidence", type=Path)
    parser.add_argument("--build-identity", type=Path)
    parser.add_argument("--vectorized", type=Path)
    parser.add_argument("--stanli-run", type=Path)
    parser.add_argument("--cmdstan-run", type=Path, action="append", default=[])
    parser.add_argument("--rerun", type=Path, action="append", default=[],
                        help="a run of the primary run's loaded models (tools/loaded_models.py)")
    parser.add_argument("--max-load", type=float,
                        help=f"load threshold for --rerun (default {DEFAULT_MAX_LOAD:g})")
    args = parser.parse_args()
    if args.max_load is not None and not args.rerun:
        parser.error("--max-load needs --rerun")
    if args.vectorized:
        if args.summary or not args.stanli_run or not args.cmdstan_run:
            parser.error("--vectorized needs --stanli-run and --cmdstan-run and no positional arguments")
        publish_vectorized(args.vectorized, args.stanli_run, args.cmdstan_run)
    elif args.summary and args.output:
        publish(args.summary, args.output, args.compiler_evidence, args.build_identity,
                args.rerun, DEFAULT_MAX_LOAD if args.max_load is None else args.max_load)
    else:
        parser.error("RUN.tsv and OUTPUT_DIRECTORY are required")


if __name__ == "__main__":
    main()
