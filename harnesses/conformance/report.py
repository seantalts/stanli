"""Machine report, generated unsupported report, and snapshot gates."""

from __future__ import annotations

import collections
import dataclasses
import datetime as _datetime
import hashlib
import json
import pathlib
import re
import shutil
import tempfile
from typing import Dict, List, Mapping, Optional, Tuple

from .status import BLOCKING_STATUSES, CaseResult, ResultStatus


REPORT_SCHEMA_VERSION = 1
SNAPSHOT_SCHEMA_VERSION = 1


class ReportError(ValueError):
    pass


class SnapshotRefused(ReportError):
    pass


@dataclasses.dataclass(frozen=True)
class Scope:
    complete: bool
    selected_cases: int
    total_cases: int
    case: Optional[str] = None
    filter: Optional[str] = None
    shard: Optional[str] = None

    def to_dict(self) -> Dict[str, object]:
        value: Dict[str, object] = {
            "complete": self.complete,
            "selected_cases": self.selected_cases,
            "total_cases": self.total_cases,
        }
        for key in ("case", "filter", "shard"):
            item = getattr(self, key)
            if item is not None:
                value[key] = item
        return value

    @classmethod
    def from_dict(cls, value: Mapping[str, object]) -> "Scope":
        return cls(
            complete=bool(value["complete"]),
            selected_cases=int(value["selected_cases"]),
            total_cases=int(value["total_cases"]),
            case=str(value["case"]) if value.get("case") is not None else None,
            filter=(str(value["filter"])
                    if value.get("filter") is not None else None),
            shard=(str(value["shard"])
                   if value.get("shard") is not None else None),
        )


@dataclasses.dataclass(frozen=True)
class SnapshotDelta:
    baseline_present: bool
    required: bool = False
    new_ids: Tuple[str, ...] = ()
    missing_ids: Tuple[str, ...] = ()
    changed_ids: Tuple[str, ...] = ()
    metadata_changes: Tuple[str, ...] = ()

    @property
    def stale(self) -> bool:
        return ((self.required and not self.baseline_present)
                or bool(self.new_ids)
                or bool(self.missing_ids) or bool(self.changed_ids)
                or bool(self.metadata_changes))

    def to_dict(self) -> Dict[str, object]:
        return {
            "baseline_present": self.baseline_present,
            "required": self.required,
            "stale": self.stale,
            "new_ids": list(self.new_ids),
            "missing_ids": list(self.missing_ids),
            "changed_ids": list(self.changed_ids),
            "metadata_changes": list(self.metadata_changes),
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, object]) -> "SnapshotDelta":
        return cls(
            baseline_present=bool(value.get("baseline_present", False)),
            required=bool(value.get("required", False)),
            new_ids=tuple(str(x) for x in value.get("new_ids", [])),
            missing_ids=tuple(str(x) for x in value.get("missing_ids", [])),
            changed_ids=tuple(str(x) for x in value.get("changed_ids", [])),
            metadata_changes=tuple(str(x)
                                   for x in value.get("metadata_changes", [])),
        )


@dataclasses.dataclass(frozen=True)
class ConformanceReport:
    created_at: str
    inventory: Mapping[str, object]
    policy: Mapping[str, object]
    scope: Scope
    tools: Mapping[str, object]
    results: Tuple[CaseResult, ...]
    policy_stale_rules: Tuple[str, ...] = ()
    snapshot_delta: SnapshotDelta = dataclasses.field(
        default_factory=lambda: SnapshotDelta(False))
    extra_gate_issues: Tuple[str, ...] = ()

    def __post_init__(self) -> None:
        ids = [result.case_id for result in self.results]
        duplicates = sorted(case_id for case_id, count
                            in collections.Counter(ids).items() if count > 1)
        if duplicates:
            raise ReportError("duplicate case IDs: " + ", ".join(duplicates[:8]))
        if self.scope.selected_cases != len(self.results):
            raise ReportError(
                f"scope selected_cases={self.scope.selected_cases}, "
                f"but report has {len(self.results)} results")
        if self.scope.complete and self.scope.total_cases != len(self.results):
            raise ReportError("complete scope does not contain every case")

    @property
    def status_counts(self) -> Mapping[str, int]:
        counts = collections.Counter(result.status.value for result in self.results)
        return {status.value: counts.get(status.value, 0)
                for status in ResultStatus}

    @property
    def policy_improvements(self) -> Tuple[str, ...]:
        return tuple(sorted(result.case_id for result in self.results
                            if result.status == ResultStatus.VERIFIED
                            and result.policy_rule is not None))

    @property
    def gate_issues(self) -> Tuple[str, ...]:
        issues: List[str] = list(self.extra_gate_issues)
        if not self.scope.complete:
            issues.append("partial_run")
        for status in sorted(BLOCKING_STATUSES, key=lambda x: x.value):
            count = self.status_counts[status.value]
            if count:
                issues.append(f"{status.value}:{count}")
        if self.policy_stale_rules:
            issues.append("stale_policy_rules:" + ",".join(self.policy_stale_rules))
        if self.policy_improvements:
            issues.append(f"policy_improvements:{len(self.policy_improvements)}")
        if self.snapshot_delta.stale:
            issues.append("snapshot_stale")
        return tuple(issues)

    @property
    def green(self) -> bool:
        return not self.gate_issues

    def to_dict(self) -> Dict[str, object]:
        return {
            "schema_version": REPORT_SCHEMA_VERSION,
            "created_at": self.created_at,
            "green": self.green,
            "gate_issues": list(self.gate_issues),
            "inventory": dict(self.inventory),
            "policy": dict(self.policy),
            "policy_stale_rules": list(self.policy_stale_rules),
            "policy_improvements": list(self.policy_improvements),
            "scope": self.scope.to_dict(),
            "tools": dict(self.tools),
            "status_counts": dict(self.status_counts),
            "snapshot_delta": self.snapshot_delta.to_dict(),
            "extra_gate_issues": list(self.extra_gate_issues),
            "results": [result.to_dict() for result in self.results],
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, object]) -> "ConformanceReport":
        schema = int(value.get("schema_version", 0))
        if schema != REPORT_SCHEMA_VERSION:
            raise ReportError(f"unsupported report schema_version {schema}")
        return cls(
            created_at=str(value["created_at"]),
            inventory=dict(value["inventory"]),
            policy=dict(value["policy"]),
            scope=Scope.from_dict(value["scope"]),
            tools=dict(value.get("tools", {})),
            results=tuple(CaseResult.from_dict(x)
                          for x in value.get("results", [])),
            policy_stale_rules=tuple(
                str(x) for x in value.get("policy_stale_rules", [])),
            snapshot_delta=SnapshotDelta.from_dict(
                value.get("snapshot_delta", {"baseline_present": False})),
            # Gate issues derived from status/scope/snapshot are intentionally
            # not copied back as extra issues.
            extra_gate_issues=tuple(
                str(x) for x in value.get("extra_gate_issues", [])),
        )


def utc_timestamp() -> str:
    return _datetime.datetime.now(_datetime.timezone.utc).replace(
        microsecond=0).isoformat().replace("+00:00", "Z")


def load_report(path: pathlib.Path) -> ConformanceReport:
    try:
        value = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReportError(f"could not read report {path}: {exc}") from exc
    return ConformanceReport.from_dict(value)


def write_report(report: ConformanceReport, path: pathlib.Path) -> None:
    _atomic_write(path, json.dumps(report.to_dict(), indent=2, sort_keys=True)
                  + "\n")


def _classification_record(result: CaseResult) -> Dict[str, object]:
    # Numeric measurements and case-specific prose do not belong in the
    # baseline.  Non-numeric omissions do: changing their reason or reviewed
    # rule is exactly the change the snapshot is meant to expose.
    record: Dict[str, object] = {"status": result.status.value}
    if result.status in (ResultStatus.EXPECTED_UNSUPPORTED,
                         ResultStatus.INAPPLICABLE):
        record["reason"] = result.reason
    if result.policy_rule is not None:
        record["policy_rule"] = result.policy_rule
    if result.policy_reason is not None:
        record["policy_reason"] = result.policy_reason
    return record


def live_classifications(report: ConformanceReport) \
        -> Mapping[str, Dict[str, object]]:
    grouped: Dict[str, List[CaseResult]] = collections.defaultdict(list)
    for result in report.results:
        grouped[result.inventory_id].append(result)
    classifications: Dict[str, Dict[str, object]] = {}
    for inventory_id, results in grouped.items():
        records = [_classification_record(result) for result in results]
        first = records[0]
        if all(record == first for record in records[1:]):
            classifications[inventory_id] = first
            continue
        statuses = {result.status for result in results}
        # A red inventory item can have several independent generated cases.
        # Collapse it to a deterministic blocking classification for snapshot
        # comparison; snapshot creation is refused before this can be written.
        blocking_precedence = (
            ResultStatus.HARNESS_ERROR,
            ResultStatus.MISMATCH,
            ResultStatus.UNEXPECTED_UNSUPPORTED,
            ResultStatus.GENERATOR_GAP,
        )
        blocking = next((status for status in blocking_precedence
                         if status in statuses), None)
        if blocking is not None:
            classifications[inventory_id] = {"status": blocking.value}
            continue
        raise ReportError(
            f"{inventory_id}: cases have incompatible nonblocking "
            "classifications: "
            + ", ".join(sorted(status.value for status in statuses)))
    return dict(sorted(classifications.items()))


def snapshot_for(report: ConformanceReport) -> Dict[str, object]:
    reasons = []
    if not report.scope.complete:
        reasons.append("run is partial")
    blockers = [result.case_id for result in report.results
                if result.status in BLOCKING_STATUSES]
    if blockers:
        reasons.append(f"run has {len(blockers)} blocking results")
    harness_errors = [result.case_id for result in report.results
                      if result.status == ResultStatus.HARNESS_ERROR]
    if harness_errors:
        reasons.append(f"run has {len(harness_errors)} harness errors")
    if report.policy_stale_rules:
        reasons.append("policy has stale rules")
    if report.policy_improvements:
        reasons.append("expected-unsupported cases now verify")
    if report.extra_gate_issues:
        reasons.append("aggregate or validation gate has errors")
    if reasons:
        raise SnapshotRefused("snapshot update refused: " + "; ".join(reasons))
    classifications = live_classifications(report)
    expected = int(report.inventory.get("total_signatures", 0))
    expected = int(report.inventory.get("total_items", expected))
    if len(classifications) != expected:
        raise SnapshotRefused(
            "snapshot update refused: inventory/result count mismatch "
            f"({len(classifications)} vs {expected})")
    return {
        "schema_version": SNAPSHOT_SCHEMA_VERSION,
        "inventory": {
            "stanc_build_id": report.inventory.get("stanc_build_id"),
            "stanc_sha256": report.inventory.get("stanc_sha256"),
            "raw_sha256": report.inventory.get("raw_sha256"),
            "total_signatures": report.inventory.get("total_signatures"),
            "total_names": report.inventory.get("total_names"),
            "total_constructs": report.inventory.get("total_constructs", 0),
            "total_items": report.inventory.get(
                "total_items", report.inventory.get("total_signatures")),
            "construct_catalog_sha256": dict(
                report.inventory.get("construct_catalog", {})).get("sha256"),
        },
        "policy": {
            "policy_version": report.policy.get("policy_version"),
            "sha256": report.policy.get("sha256"),
        },
        "classifications": classifications,
    }


def write_snapshot(report: ConformanceReport, path: pathlib.Path) -> None:
    value = snapshot_for(report)
    _atomic_write(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def load_snapshot(path: pathlib.Path) -> Optional[Mapping[str, object]]:
    path = pathlib.Path(path)
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReportError(f"could not read baseline {path}: {exc}") from exc
    if int(value.get("schema_version", 0)) != SNAPSHOT_SCHEMA_VERSION:
        raise ReportError(f"unsupported snapshot schema_version in {path}")
    if not isinstance(value.get("classifications"), dict):
        raise ReportError(f"snapshot {path} has no classifications object")
    return value


def compare_snapshot(report: ConformanceReport,
                     snapshot: Optional[Mapping[str, object]],
                     required: bool = False) -> SnapshotDelta:
    if snapshot is None:
        return SnapshotDelta(False, required=required)
    live = live_classifications(report)
    baseline = dict(snapshot.get("classifications", {}))
    live_ids, baseline_ids = set(live), set(baseline)
    changed = tuple(sorted(case_id for case_id in live_ids & baseline_ids
                           if live[case_id] != baseline[case_id]))
    metadata_changes = []
    for section, keys in (("inventory", ("stanc_build_id", "stanc_sha256",
                                          "raw_sha256", "total_signatures",
                                          "total_names", "total_constructs",
                                          "total_items",
                                          "construct_catalog_sha256")),
                          ("policy", ("policy_version", "sha256"))):
        old = dict(snapshot.get(section, {}))
        if section == "inventory":
            current = dict(report.inventory)
            current.setdefault("total_constructs", 0)
            current.setdefault("total_items",
                               current.get("total_signatures"))
            current["construct_catalog_sha256"] = dict(
                report.inventory.get("construct_catalog", {})).get("sha256")
        else:
            current = report.policy
        for key in keys:
            if old.get(key) != current.get(key):
                metadata_changes.append(f"{section}.{key}")
    return SnapshotDelta(
        True, required=required,
        new_ids=tuple(sorted(live_ids - baseline_ids)),
        missing_ids=(tuple(sorted(baseline_ids - live_ids))
                     if report.scope.complete else ()),
        changed_ids=changed,
        metadata_changes=tuple(metadata_changes),
    )


def with_snapshot(report: ConformanceReport,
                  snapshot: Optional[Mapping[str, object]],
                  required: bool = False) -> ConformanceReport:
    return dataclasses.replace(report,
                               snapshot_delta=compare_snapshot(
                                   report, snapshot, required=required))


_SECTION_TITLES = {
    ResultStatus.EXPECTED_UNSUPPORTED: "Expected unsupported",
    ResultStatus.UNEXPECTED_UNSUPPORTED: "Unexpected unsupported",
    ResultStatus.INAPPLICABLE: "Inapplicable",
    ResultStatus.GENERATOR_GAP: "Generator gaps",
    ResultStatus.MISMATCH: "Mismatches",
    ResultStatus.HARNESS_ERROR: "Harness errors",
}


def _md_text(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def render_markdown(report: ConformanceReport) -> str:
    counts = report.status_counts
    lines = [
        "<!-- Generated by harnesses/stan_conformance.py; do not edit. -->",
        "# Stan conformance inventory",
        "",
        f"**Gate: {'GREEN' if report.green else 'RED'}**",
        "",
        f"stanc: `{_md_text(report.inventory.get('stanc_build_id', 'unknown'))}`  ",
        f"Inventory: {report.inventory.get('total_signatures', 0)} signatures "
        f"under {report.inventory.get('total_names', 0)} names and "
        f"{report.inventory.get('total_constructs', 0)} named constructs  ",
        f"Scope: {report.scope.selected_cases}/{report.scope.total_cases} cases"
        + (" (complete)" if report.scope.complete else " (partial)"),
        "",
        "| Status | Count | Blocking |",
        "| --- | ---: | :---: |",
    ]
    for status in ResultStatus:
        lines.append(f"| `{status.value}` | {counts[status.value]} | "
                     f"{'yes' if status.is_blocking else 'no'} |")
    lines.extend(["", "Gate issues: "
                  + (", ".join(f"`{x}`" for x in report.gate_issues)
                     if report.gate_issues else "none"), ""])

    if report.policy_improvements:
        lines.extend(["## Policy improvements", "",
                      "These cases now verify, so their capability exceptions "
                      "must be removed before the snapshot can be refreshed.", ""])
        lines.extend(f"- `{case_id}`" for case_id in report.policy_improvements)
        lines.append("")

    for status, title in _SECTION_TITLES.items():
        selected = [result for result in report.results
                    if result.status == status]
        lines.extend([f"## {title} ({len(selected)})", ""])
        if not selected:
            lines.extend(["None.", ""])
            continue
        by_reason: Dict[str, List[CaseResult]] = collections.defaultdict(list)
        for result in selected:
            by_reason[result.reason].append(result)
        for reason in sorted(by_reason):
            group = by_reason[reason]
            lines.extend([f"### {_md_text(reason)} ({len(group)})", ""])
            by_family: Dict[str, List[CaseResult]] = collections.defaultdict(list)
            for result in group:
                by_family[result.family].append(result)
            for family in sorted(by_family):
                cases = sorted(by_family[family], key=lambda x: x.case_id)
                lines.append(f"- **`{_md_text(family)}`** ({len(cases)})")
                for result in cases:
                    suffix = (f" — reproduce: `{_md_text(result.repro_command)}`"
                              if result.repro_command else "")
                    lines.append(f"  - `{_md_text(result.case_id)}`{suffix}")
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def write_markdown(report: ConformanceReport, path: pathlib.Path) -> None:
    _atomic_write(path, render_markdown(report))


def _populate_reproducers(report: ConformanceReport,
                          staging: pathlib.Path) -> None:
    blockers = [result for result in report.results if result.status.is_blocking]
    index = []
    copied_sources: Dict[str, pathlib.Path] = {}
    source_digests: Dict[pathlib.Path, str] = {}
    for result in blockers:
        row: Dict[str, object] = {
            "case_id": result.case_id,
            "status": result.status.value,
            "reason": result.reason,
            "repro_command": result.repro_command,
            "probe_attempted": result.probe_attempted,
        }
        if result.probe_attempted:
            safe_family = re.sub(r"[^A-Za-z0-9_.-]+", "-", result.family)[:48]
            digest = hashlib.sha256(result.case_id.encode("utf-8")).hexdigest()[:16]
            case_directory = staging / f"{safe_family}-{digest}"
            case_directory.mkdir(parents=True, exist_ok=True)
            _atomic_write(case_directory / "result.json",
                          json.dumps(result.to_dict(), indent=2, sort_keys=True)
                          + "\n")
            if result.repro_command:
                _atomic_write(case_directory / "reproduce.txt",
                              result.repro_command + "\n")
            active_case = result.details.get("active_case")
            if active_case is not None:
                _atomic_write(case_directory / "data.json",
                              json.dumps({"active_case": active_case}, indent=2)
                              + "\n")
            data_path = result.details.get("data_path")
            if data_path is not None:
                data = pathlib.Path(str(data_path))
                if data.is_file():
                    shutil.copyfile(data, case_directory / "data.json")
            point_rows = []
            for point in result.details.get("points", []):
                if isinstance(point, dict) and "point" in point:
                    point_rows.append({"point_index": point.get("point_index"),
                                       "point": point["point"],
                                       "point_offset": point.get(
                                           "point_offset", 0),
                                       "point_total": point.get(
                                           "point_total", len(point["point"])),
                                       "point_encoding": point.get(
                                           "point_encoding", "full")})
            if point_rows:
                _atomic_write(case_directory / "points.json",
                              json.dumps(point_rows, indent=2) + "\n")
            source_path = result.details.get("source_path")
            if source_path is not None:
                source = pathlib.Path(str(source_path))
                if source.is_file():
                    source_sha256 = source_digests.get(source)
                    if source_sha256 is None:
                        source_sha256 = hashlib.sha256(
                            source.read_bytes()).hexdigest()
                        source_digests[source] = source_sha256
                    shared = copied_sources.get(source_sha256)
                    if shared is None:
                        shared = staging / "sources" / \
                            (source_sha256[:24] + ".stan")
                        shared.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(source, shared)
                        copied_sources[source_sha256] = shared
                    relative = pathlib.Path("..") / "sources" / shared.name
                    _atomic_write(
                        case_directory / "source.json",
                        json.dumps({"path": str(relative),
                                    "sha256": source_sha256},
                                   indent=2, sort_keys=True) + "\n")
                    row["source"] = str(pathlib.Path("sources") / shared.name)
            row["directory"] = case_directory.name
        index.append(row)
    _atomic_write(staging / "index.json",
                  json.dumps(index, indent=2, sort_keys=True) + "\n")


def write_reproducers(report: ConformanceReport, directory: pathlib.Path) -> None:
    """Write a compact index for every blocker and full probed reproducers.

    Inventory-only generator gaps number in the tens of thousands, so they
    remain one-command rows in the index.  Cases which reached an oracle get
    a directory with selector, points, both raw outcomes, and a pointer to a
    content-addressed source copied once under `sources/`; those are the
    failures a developer can execute immediately without duplicating a shard
    for every red case.
    The directory swap prevents resolved failures from leaving stale files.
    """
    directory = pathlib.Path(directory)
    directory.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(
        prefix=directory.name + ".", dir=str(directory.parent)))
    backup = None
    try:
        _populate_reproducers(report, staging)
        if directory.exists():
            backup = pathlib.Path(tempfile.mkdtemp(
                prefix=directory.name + ".old.", dir=str(directory.parent)))
            backup.rmdir()
            directory.replace(backup)
        staging.replace(directory)
    except Exception:
        if backup is not None and backup.exists() and not directory.exists():
            backup.replace(directory)
        raise
    finally:
        if staging.exists():
            shutil.rmtree(staging)
        if backup is not None and backup.exists():
            shutil.rmtree(backup)


def write_generated_sources(report: ConformanceReport,
                            directory: pathlib.Path) -> None:
    """Materialize every unique generated shard when `--keep` requested it."""
    directory = pathlib.Path(directory)
    if not bool(report.tools.get("keep", False)):
        if directory.exists():
            shutil.rmtree(directory)
        return
    directory.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(
        prefix=directory.name + ".", dir=str(directory.parent)))
    by_shard: Dict[str, Dict[str, object]] = {}
    for result in report.results:
        source_path = result.details.get("source_path")
        shard = result.details.get("shard")
        if source_path is None or not isinstance(shard, dict):
            continue
        shard_id = str(shard.get("id", ""))
        source = pathlib.Path(str(source_path))
        if not shard_id or not source.is_file():
            continue
        row = by_shard.setdefault(
            shard_id, {"source_sha256": shard.get("source_sha256"),
                       "case_ids": set(), "source": source})
        row["case_ids"].add(result.case_id)
    index = []
    backup = None
    try:
        for shard_id, row in sorted(by_shard.items()):
            filename = shard_id + ".stan"
            shutil.copyfile(row["source"], staging / filename)
            index.append({"shard": shard_id, "source": filename,
                          "source_sha256": row["source_sha256"],
                          "case_ids": sorted(row["case_ids"])})
        _atomic_write(staging / "index.json",
                      json.dumps(index, indent=2, sort_keys=True) + "\n")
        if directory.exists():
            backup = pathlib.Path(tempfile.mkdtemp(
                prefix=directory.name + ".old.", dir=str(directory.parent)))
            backup.rmdir()
            directory.replace(backup)
        staging.replace(directory)
    except Exception:
        if backup is not None and backup.exists() and not directory.exists():
            backup.replace(directory)
        raise
    finally:
        if staging.exists():
            shutil.rmtree(staging)
        if backup is not None and backup.exists():
            shutil.rmtree(backup)


def _atomic_write(path: pathlib.Path, content: str) -> None:
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", dir=str(path.parent),
            prefix=path.name + ".", suffix=".tmp", delete=False) as handle:
        handle.write(content)
        temporary = pathlib.Path(handle.name)
    # NamedTemporaryFile is 0600 by default.  Reports are ordinary build
    # artifacts and optional baselines are ordinary shareable files, so
    # preserve an existing mode or use the repository-friendly 0644 default.
    temporary.chmod(path.stat().st_mode if path.exists() else 0o644)
    temporary.replace(path)
