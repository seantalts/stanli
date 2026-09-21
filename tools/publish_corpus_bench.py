#!/usr/bin/env python3
"""Publish a complete v4 corpus run without rerunning measurements.

python3 tools/publish_corpus_bench.py RUN.tsv OUTPUT_DIRECTORY
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from corpus_table import render_catalog


def publish(summary, output, compiler_evidence=None, build_identity=None):
    run = Path(str(summary) + ".run")
    manifest = json.loads((run / "manifest.json").read_text())
    records = [json.loads(path.read_text())
               for path in sorted(run.glob("*.result.json"))]
    if len(records) != len(manifest["identity"]["inputs"]):
        raise ValueError("Cannot publish an incomplete corpus run")
    if output.exists():
        raise ValueError(f"Output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".corpus-publish-", dir=output.parent) as temporary:
        staging = Path(temporary) / "evidence"
        staging.mkdir()
        shutil.copyfile(summary, staging / "benchmark-summary.tsv")
        shutil.copyfile(run / "manifest.json", staging / "benchmark-manifest.json")
        for name, content in (("model-results.json.gz", json.dumps(records, allow_nan=False).encode()),
                              ("events.jsonl.gz", (run / "events.jsonl").read_bytes())):
            (staging / name).write_bytes(gzip.compress(content, mtime=0))
        table = render_catalog(staging / "benchmark-summary.tsv",
                               staging / "model-results.json.gz",
                               staging / "benchmark-manifest.json")
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
            "Estimated time is measured setup plus 20,000 times median warm gradient "
            "latency; full sampling is not run. Failures remain in the inventory.\n\n"
            f"Full inputs, generated code and command logs remain locally at `{run}`; "
            "these larger artifacts are not hosted with this report.\n\n" + table)
        checksums = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}"
                     for path in sorted(staging.iterdir()) if path.is_file()]
        (staging / "SHA256SUMS").write_text("\n".join(checksums) + "\n")
        staging.rename(output)
    print(f"Published {len(records)} outcomes from {manifest['run_id']} to {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler-evidence", type=Path)
    parser.add_argument("--build-identity", type=Path)
    args = parser.parse_args()
    publish(args.summary, args.output, args.compiler_evidence, args.build_identity)


if __name__ == "__main__":
    main()
