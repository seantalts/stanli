#!/usr/bin/env python3
"""One entry point for generated Stan language differential conformance.

Discovery is exhaustive and classification remains honest: applicable
signatures without a type-directed case are generator_gap (red), never inferred
unsupported from stanli implementation tables. Generated numeric shards and
the named construct catalog replace rows only with observed oracle results.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import sys
from typing import Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "harnesses"))

from conformance.aggregate import aggregate_reports  # noqa: E402
from conformance.catalog import (CatalogError,  # noqa: E402
                                 load_construct_catalog)
from conformance.construct_runner import run_construct_phase  # noqa: E402
from conformance.oracle import validate_toolchain  # noqa: E402
from conformance.policy import PolicyError, load_policy  # noqa: E402
from conformance.report import (  # noqa: E402
    ReportError,
    SnapshotRefused,
    load_report,
    load_snapshot,
    with_snapshot,
    write_markdown,
    write_generated_sources,
    write_reproducers,
    write_report,
    write_snapshot,
)
from conformance.runner import (  # noqa: E402
    Selection,
    partition_manifest,
    run_inventory,
)
from conformance.scalar_runner import run_scalar_phase  # noqa: E402
from conformance.signatures import SignatureParseError, load_inventory  # noqa: E402


DEFAULT_POLICY = REPO / "harnesses" / "conformance" / "policy.toml"
DEFAULT_CONSTRUCTS = REPO / "harnesses" / "conformance" / "constructs.toml"


def _shard(value: str) -> Tuple[int, int]:
    try:
        left, right = value.split("/", 1)
        index, count = int(left), int(right)
        Selection(shard_index=index, shard_count=count)
        return index, count
    except (ValueError, TypeError) as exc:
        raise argparse.ArgumentTypeError(
            "shard must be one-based N/M, for example 1/8") from exc


class _Once(argparse.Action):
    """Refuse a repeated selector instead of silently keeping the last one.

    Mutual exclusion catches --case beside --filter, but a second --case is
    only an assignment over the first, so `--case A --case B` runs B and
    never mentions A. Selecting cases is the commonest interactive use of
    this harness, and a dropped selector does not look like a dropped
    selector: it looks like the harness disagreeing about the case you named
    first. One case per invocation; --filter takes a group.
    """

    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest) is not None:
            parser.error(f"{option_string} may be given only once "
                         "(it is not repeatable; use --filter for a group)")
        setattr(namespace, self.dest, values)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Discover and differentially classify Stan signatures")
    parser.add_argument("--stanc", type=pathlib.Path,
                        help="pinned stanc3 executable")
    parser.add_argument("--cmdstan", type=pathlib.Path,
                        help="pinned CmdStan checkout (needed by numeric phases)")
    parser.add_argument("--build", type=pathlib.Path,
                        default=pathlib.Path("build-rel"),
                        help="stanli build tree")
    parser.add_argument("--policy", type=pathlib.Path, default=DEFAULT_POLICY)
    parser.add_argument("--constructs", type=pathlib.Path,
                        default=DEFAULT_CONSTRUCTS,
                        help="named non-function language construct catalog")
    parser.add_argument("--baseline", type=pathlib.Path,
                        help="optional generated snapshot to enforce")
    parser.add_argument("--output", type=pathlib.Path,
                        default=pathlib.Path("conformance-out"))
    parser.add_argument("--mode", choices=("auto", "inventory", "scalar"),
                        default="auto",
                        help="auto runs scalar parity when --cmdstan is given")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--case", action=_Once,
                           help="one exact canonical or case ID")
    selection.add_argument("--filter", action=_Once,
                           help="case-insensitive signature substring")
    parser.add_argument("--shard", type=_shard,
                        help="stable one-based distributed partition N/M")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="seconds per long-lived protocol request")
    parser.add_argument("--scalar-shard-size", type=int, default=128)
    parser.add_argument("--stanli-python", type=pathlib.Path,
                        default=pathlib.Path(sys.executable),
                        help="Python interpreter with stanli installed")
    parser.add_argument("--stanli-pythonpath", type=pathlib.Path,
                        help="optional package root prepended for the worker")
    parser.add_argument("--resume", type=pathlib.Path,
                        help="artifact cache to reuse in later numeric phases")
    parser.add_argument("--keep", action="store_true",
                        help="retain generated sources in later numeric phases")
    parser.add_argument("--update-snapshot", action="store_true")
    parser.add_argument("--report-only", action="store_true",
                        help="regenerate Markdown from an existing JSON report")
    parser.add_argument("--input", type=pathlib.Path,
                        help="input report for --report-only")
    parser.add_argument("--write-manifest", type=int, metavar="M",
                        help="write a deterministic M-partition manifest and exit")
    parser.add_argument("--aggregate", nargs="+", type=pathlib.Path,
                        metavar="REPORT",
                        help="merge partition reports instead of evaluating")
    parser.add_argument("--manifest", type=pathlib.Path,
                        help="partition manifest for --aggregate")
    parser.add_argument("--failure-limit", type=int, default=40,
                        help="maximum blocking rows printed (all remain in JSON)")
    return parser


def _write_outputs(report, output: pathlib.Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    write_report(report, output / "conformance.json")
    write_markdown(report, output / "unsupported.md")
    write_reproducers(report, output / "repro")
    write_generated_sources(report, output / "generated")
    raw_dump = report.inventory.get("raw_dump")
    if isinstance(raw_dump, str):
        (output / "signatures.txt").write_text(raw_dump, encoding="utf-8")


def _with_configured_snapshot(report, args):
    """Apply snapshot parity only when the caller explicitly configured it."""
    snapshot = load_snapshot(args.baseline) if args.baseline is not None else None
    return with_snapshot(report, snapshot, required=args.baseline is not None)


def _update_configured_snapshot(report, args):
    if args.baseline is None:
        raise ReportError("--update-snapshot requires --baseline PATH")
    write_snapshot(report, args.baseline)
    return with_snapshot(report, load_snapshot(args.baseline), required=True)


def _print_summary(report, output: pathlib.Path, failure_limit: int) -> None:
    counts = report.status_counts
    summary = ", ".join(f"{name}={count}" for name, count in counts.items()
                        if count)
    print(f"{'GREEN' if report.green else 'RED'}: {summary or 'no cases'}")
    blocking = [result for result in report.results if result.status.is_blocking]
    for result in blocking[:max(0, failure_limit)]:
        print(f"FAIL {result.status.value} {result.case_id}: {result.reason}")
        if result.repro_command:
            print(f"  reproduce: {result.repro_command}")
    if len(blocking) > max(0, failure_limit):
        print(f"... {len(blocking) - max(0, failure_limit)} more blocking "
              f"results in {output / 'conformance.json'}")
    print("gate issues: " + (", ".join(report.gate_issues) or "none"))
    print(f"report: {output / 'conformance.json'}")
    print(f"unsupported: {output / 'unsupported.md'}")


def _report_only(args) -> int:
    source = args.input or args.output / "conformance.json"
    report = load_report(source)
    report = _with_configured_snapshot(report, args)
    # Preserve the failed pre-update run before attempting a snapshot write.
    _write_outputs(report, args.output)
    if args.update_snapshot:
        report = _update_configured_snapshot(report, args)
    _write_outputs(report, args.output)
    _print_summary(report, args.output, args.failure_limit)
    return 0 if report.green else 1


def _aggregate(args) -> int:
    if args.manifest is None:
        raise ReportError("--aggregate requires --manifest")
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    reports = [load_report(path) for path in args.aggregate]
    report = aggregate_reports(reports, manifest)
    report = _with_configured_snapshot(report, args)
    _write_outputs(report, args.output)
    if args.update_snapshot:
        report = _update_configured_snapshot(report, args)
    _write_outputs(report, args.output)
    _print_summary(report, args.output, args.failure_limit)
    return 0 if report.green else 1


def _run(args) -> int:
    if args.stanc is None:
        raise ReportError("--stanc is required for inventory runs")
    if args.jobs < 1:
        raise ReportError("--jobs must be positive")
    if args.failure_limit < 0:
        raise ReportError("--failure-limit must be nonnegative")
    if args.timeout <= 0:
        raise ReportError("--timeout must be positive")
    if args.scalar_shard_size < 1:
        raise ReportError("--scalar-shard-size must be positive")
    inventory = load_inventory(args.stanc)
    catalog = load_construct_catalog(args.constructs, REPO)
    if args.write_manifest is not None:
        if args.write_manifest < 1:
            raise ReportError("--write-manifest requires a positive count")
        args.output.mkdir(parents=True, exist_ok=True)
        manifest = partition_manifest(inventory, args.write_manifest, catalog)
        if args.cmdstan is not None:
            versions = validate_toolchain(args.cmdstan, args.stanc,
                                          REPO / "deps")
            manifest["reference_toolchain"] = versions.to_dict()
        path = args.output / "partition-manifest.json"
        path.write_text(json.dumps(manifest,
                                   indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
        print(path)
        return 0

    policy = load_policy(args.policy)
    shard_index, shard_count = args.shard or (None, None)
    selection = Selection(case=args.case, filter=args.filter,
                          shard_index=shard_index, shard_count=shard_count)
    report = run_inventory(
        inventory, policy, selection, args.stanc, args.cmdstan, args.build,
        REPO, jobs=args.jobs, catalog=catalog)
    if not report.results:
        raise ReportError("selection matched no inventory signatures")
    tool_details = dict(report.tools)
    tool_details.update({"keep": args.keep,
                         "resume": str(args.resume) if args.resume else None})
    report = dataclasses.replace(report, tools=tool_details)

    mode = ("scalar" if args.mode == "auto" and args.cmdstan is not None
            else "inventory" if args.mode == "auto" else args.mode)
    if mode == "scalar":
        if args.cmdstan is None:
            raise ReportError("--mode scalar requires --cmdstan")
        cache_root = args.resume or args.output / "cache"
        report = run_scalar_phase(
            report, inventory, policy, args.cmdstan, args.stanc, args.build,
            REPO / "deps", cache_root, python_executable=args.stanli_python,
            pythonpath=args.stanli_pythonpath,
            shard_size=args.scalar_shard_size, jobs=args.jobs,
            timeout=args.timeout)
        report = run_construct_phase(
            report, catalog, policy, args.cmdstan, args.stanc, REPO / "deps",
            cache_root, REPO, python_executable=args.stanli_python,
            pythonpath=args.stanli_pythonpath,
            jobs=args.jobs, timeout=args.timeout)

    report = _with_configured_snapshot(report, args)
    _write_outputs(report, args.output)
    if args.update_snapshot:
        # The update gate intentionally ignores whether the old snapshot is
        # stale; a complete reviewed green run is how it is refreshed.
        report = _update_configured_snapshot(report, args)
    _write_outputs(report, args.output)
    _print_summary(report, args.output, args.failure_limit)
    return 0 if report.green else 1


def main(argv=None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.report_only:
            if args.aggregate:
                raise ReportError("--report-only and --aggregate are exclusive")
            return _report_only(args)
        if args.aggregate:
            return _aggregate(args)
        return _run(args)
    except (OSError, json.JSONDecodeError, CatalogError, PolicyError, ReportError,
            SignatureParseError, SnapshotRefused, RuntimeError,
            ValueError) as exc:
        print(f"HARNESS ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
