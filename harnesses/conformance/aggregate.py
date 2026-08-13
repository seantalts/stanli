"""Strict aggregation of distributed conformance partitions."""

from __future__ import annotations

import collections
from typing import List, Mapping, Optional, Sequence, Tuple

from .report import ConformanceReport, ReportError, Scope, utc_timestamp


def manifest_case_ids(manifest: Mapping[str, object]) -> Tuple[str, ...]:
    if int(manifest.get("schema_version", 0)) != 1:
        raise ReportError("unsupported partition manifest schema_version")
    partitions = manifest.get("partitions")
    if not isinstance(partitions, list) or not partitions:
        raise ReportError("partition manifest has no partitions")
    case_ids: List[str] = []
    shard_names = set()
    for partition in partitions:
        if not isinstance(partition, dict):
            raise ReportError("partition manifest row is not an object")
        shard = str(partition.get("shard", ""))
        if not shard or shard in shard_names:
            raise ReportError(f"duplicate or empty manifest shard {shard!r}")
        shard_names.add(shard)
        ids = partition.get("case_ids")
        if not isinstance(ids, list):
            raise ReportError(f"manifest shard {shard} has no case_ids array")
        if int(partition.get("count", -1)) != len(ids):
            raise ReportError(f"manifest shard {shard} count is inconsistent")
        case_ids.extend(str(case_id) for case_id in ids)
    duplicates = sorted(case_id for case_id, count
                        in collections.Counter(case_ids).items() if count > 1)
    if duplicates:
        raise ReportError("manifest assigns duplicate case IDs: "
                          + ", ".join(duplicates[:8]))
    if int(manifest.get("total_cases", -1)) != len(case_ids):
        raise ReportError("manifest total_cases is inconsistent")
    return tuple(case_ids)


def aggregate_reports(reports: Sequence[ConformanceReport],
                      manifest: Mapping[str, object],
                      created_at: Optional[str] = None) -> ConformanceReport:
    if not reports:
        raise ReportError("no partition reports were provided")
    expected = manifest_case_ids(manifest)
    first = reports[0]
    manifest_digest = str(manifest.get("inventory_sha256", ""))
    if manifest_digest != first.inventory.get("raw_sha256"):
        raise ReportError("partition manifest inventory digest does not match report")
    for manifest_key, report_key in (("stanc_build_id", "stanc_build_id"),
                                     ("stanc_sha256", "stanc_sha256")):
        if (manifest.get(manifest_key) is not None
                and manifest.get(manifest_key) != first.inventory.get(report_key)):
            raise ReportError(
                f"partition manifest {manifest_key} does not match report")
    catalog_metadata = dict(first.inventory.get("construct_catalog", {}))
    if (manifest.get("construct_catalog_sha256") is not None
            and manifest.get("construct_catalog_sha256")
            != catalog_metadata.get("sha256")):
        raise ReportError(
            "partition manifest construct catalog does not match report")
    for index, report in enumerate(reports[1:], 2):
        if report.inventory != first.inventory:
            raise ReportError(f"partition report {index} has a different inventory")
        if report.policy != first.policy:
            raise ReportError(f"partition report {index} has a different policy")

    by_id = collections.defaultdict(list)
    for report in reports:
        for result in report.results:
            by_id[result.case_id].append(result)
    duplicates = tuple(sorted(case_id for case_id, values in by_id.items()
                              if len(values) > 1))
    expected_set = set(expected)
    actual_set = set(by_id)
    missing = tuple(sorted(expected_set - actual_set))
    unexpected = tuple(sorted(actual_set - expected_set))

    # Keep one copy so the aggregate remains inspectable, but make duplicate
    # and unexpected input independently blocking gate issues.
    results = tuple(sorted((values[0] for values in by_id.values()),
                           key=lambda result: result.case_id))
    issues = []
    if duplicates:
        issues.append(f"duplicate_case_ids:{len(duplicates)}")
    if missing:
        issues.append(f"missing_case_ids:{len(missing)}")
    if unexpected:
        issues.append(f"unexpected_case_ids:{len(unexpected)}")
    complete = not issues and len(results) == len(expected)
    tools = {
        "aggregate": True,
        "partition_reports": len(reports),
        "partition_tools": [dict(report.tools) for report in reports],
    }
    stale_rules = tuple(sorted({rule for report in reports
                                for rule in report.policy_stale_rules}))
    return ConformanceReport(
        created_at=created_at or utc_timestamp(),
        inventory=first.inventory,
        policy=first.policy,
        scope=Scope(complete=complete, selected_cases=len(results),
                    total_cases=len(expected)),
        tools=tools,
        results=results,
        policy_stale_rules=stale_rules,
        extra_gate_issues=tuple(issues),
    )
