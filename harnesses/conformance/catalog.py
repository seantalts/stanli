"""Checked-in inventory for Stan semantics which are not Math signatures."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import re
from typing import Dict, Mapping, Optional, Tuple

from .toml_compat import loads as toml_loads


class CatalogError(ValueError):
    pass


GATES = frozenset({"numeric", "semantic", "write_array", "compile_refusal",
                   "census"})
PHASES = frozenset({"compile", "construction", "evaluation", "write_array"})
CASE_FIELDS = frozenset({
    "id", "category", "description", "source", "data", "oracle_gate",
    "expected_accept", "expected_phase", "expected_exception", "points",
    "point_profile",
    "census_reason",
    "numeric_policy",
})


@dataclasses.dataclass(frozen=True)
class ConstructCase:
    id: str
    category: str
    description: str
    source: pathlib.Path
    data: pathlib.Path
    oracle_gate: str
    expected_accept: bool
    expected_phase: Optional[str]
    expected_exception: Optional[str]
    points: Tuple[Tuple[float, ...], ...]
    point_profile: Optional[str]
    census_reason: Optional[str]
    numeric_policy: Optional[str]
    source_sha256: str
    data_sha256: str

    @property
    def case_id(self) -> str:
        return "construct:" + self.id

    @property
    def inventory_id(self) -> str:
        return self.case_id

    def to_dict(self, repo: pathlib.Path) -> Dict[str, object]:
        def display(path: pathlib.Path) -> str:
            try:
                return str(path.relative_to(repo))
            except ValueError:
                return str(path)

        return {
            "id": self.id,
            "case_id": self.case_id,
            "category": self.category,
            "description": self.description,
            "source": display(self.source),
            "data": display(self.data),
            "oracle_gate": self.oracle_gate,
            "expected_accept": self.expected_accept,
            "expected_phase": self.expected_phase,
            "expected_exception": self.expected_exception,
            "points": [list(point) for point in self.points],
            "point_profile": self.point_profile,
            "census_reason": self.census_reason,
            "numeric_policy": self.numeric_policy,
            "source_sha256": self.source_sha256,
            "data_sha256": self.data_sha256,
        }


@dataclasses.dataclass(frozen=True)
class ConstructCatalog:
    schema_version: int
    catalog_version: str
    cases: Tuple[ConstructCase, ...]
    sha256: str
    source: str
    repo: pathlib.Path

    @property
    def count(self) -> int:
        return len(self.cases)

    @property
    def category_count(self) -> int:
        return len({case.category for case in self.cases})

    def to_metadata(self) -> Dict[str, object]:
        return {
            "schema_version": self.schema_version,
            "catalog_version": self.catalog_version,
            "sha256": self.sha256,
            "case_count": self.count,
            "category_count": self.category_count,
        }


def _known_fields(value: Mapping[str, object], permitted, label: str) -> None:
    unknown = sorted(set(value) - set(permitted))
    if unknown:
        raise CatalogError(f"{label}: unknown fields: {', '.join(unknown)}")


def _path(raw: object, repo: pathlib.Path, label: str) -> pathlib.Path:
    text = str(raw or "").strip()
    if not text:
        raise CatalogError(f"{label} is required")
    path = pathlib.Path(text)
    resolved = (repo / path).resolve() if not path.is_absolute() else path.resolve()
    try:
        resolved.relative_to(repo.resolve())
    except ValueError as exc:
        raise CatalogError(f"{label} escapes the repository: {text}") from exc
    if not resolved.is_file():
        raise CatalogError(f"{label} does not exist: {text}")
    return resolved


def _points(raw: object, label: str) -> Tuple[Tuple[float, ...], ...]:
    if raw is None:
        return ()
    if not isinstance(raw, list):
        raise CatalogError(f"{label}: points must be an array of arrays")
    points = []
    for index, point in enumerate(raw):
        if not isinstance(point, list):
            raise CatalogError(f"{label}: point {index} is not an array")
        try:
            points.append(tuple(float(value) for value in point))
        except (TypeError, ValueError) as exc:
            raise CatalogError(
                f"{label}: point {index} contains a non-number") from exc
    widths = {len(point) for point in points}
    if len(widths) > 1:
        raise CatalogError(f"{label}: deterministic points have mixed widths")
    return tuple(points)


def load_construct_catalog(path: pathlib.Path,
                           repo: pathlib.Path) -> ConstructCatalog:
    path = pathlib.Path(path)
    repo = pathlib.Path(repo).resolve()
    source = path.read_text(encoding="utf-8")
    try:
        raw = toml_loads(source)
    except ValueError as exc:
        raise CatalogError(f"could not parse {path}: {exc}") from exc
    _known_fields(raw, {"schema_version", "catalog_version", "case"},
                  "construct catalog")
    schema = int(raw.get("schema_version", 0))
    if schema != 1:
        raise CatalogError(f"unsupported construct schema_version {schema}")
    version = str(raw.get("catalog_version", "")).strip()
    if not version:
        raise CatalogError("construct catalog_version is required")
    rows = raw.get("case", [])
    if not isinstance(rows, list) or not rows:
        raise CatalogError("construct catalog has no cases")

    cases = []
    digest = hashlib.sha256()
    digest.update(source.encode("utf-8"))
    for index, row in enumerate(rows, 1):
        if not isinstance(row, dict):
            raise CatalogError(f"construct case {index} is not a table")
        _known_fields(row, CASE_FIELDS, f"construct case {index}")
        case_id = str(row.get("id", "")).strip()
        if not re.fullmatch(r"[a-z][a-z0-9_.-]*", case_id):
            raise CatalogError(f"construct case {index}: invalid id {case_id!r}")
        category = str(row.get("category", "")).strip()
        description = str(row.get("description", "")).strip()
        if not re.fullmatch(r"[a-z][a-z0-9_]*", category) or not description:
            raise CatalogError(
                f"construct case {case_id}: valid category and description "
                "required")
        gate = str(row.get("oracle_gate", "")).strip()
        if gate not in GATES:
            raise CatalogError(f"construct case {case_id}: unknown gate {gate!r}")
        raw_accept = row.get("expected_accept", True)
        if not isinstance(raw_accept, bool):
            raise CatalogError(
                f"construct case {case_id}: expected_accept must be boolean")
        expected_accept = raw_accept
        phase = (str(row["expected_phase"])
                 if row.get("expected_phase") is not None else None)
        if phase is not None and phase not in PHASES:
            raise CatalogError(
                f"construct case {case_id}: unknown phase {phase!r}")
        exception = (str(row["expected_exception"])
                     if row.get("expected_exception") is not None else None)
        if not expected_accept and phase is None:
            raise CatalogError(
                f"construct case {case_id}: rejection needs expected_phase")
        source_path = _path(row.get("source"), repo,
                            f"construct case {case_id} source")
        data_path = _path(row.get("data"), repo,
                          f"construct case {case_id} data")
        points = _points(row.get("points"), f"construct case {case_id}")
        profile = (str(row["point_profile"]).strip()
                   if row.get("point_profile") is not None else None)
        if profile == "":
            raise CatalogError(
                f"construct case {case_id}: point_profile must not be empty")
        census_reason = (str(row["census_reason"]).strip()
                         if row.get("census_reason") is not None else None)
        if gate == "census" and not census_reason:
            raise CatalogError(
                f"construct case {case_id}: census_reason is required")
        if gate != "census" and census_reason is not None:
            raise CatalogError(
                f"construct case {case_id}: census_reason requires census gate")
        numeric_policy = (str(row["numeric_policy"]).strip()
                          if row.get("numeric_policy") is not None else None)
        if numeric_policy == "":
            raise CatalogError(
                f"construct case {case_id}: numeric_policy must not be empty")
        if numeric_policy is not None and gate not in {"numeric", "write_array"}:
            raise CatalogError(
                f"construct case {case_id}: numeric_policy requires a numeric "
                "or write_array gate")
        if gate in {"numeric", "write_array", "semantic"} \
                and not points and not profile:
            raise CatalogError(
                f"construct case {case_id}: points or point_profile required")
        source_bytes, data_bytes = source_path.read_bytes(), data_path.read_bytes()
        try:
            data_value = json.loads(data_bytes)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise CatalogError(
                f"construct case {case_id}: data is not valid JSON") from exc
        if not isinstance(data_value, dict):
            raise CatalogError(
                f"construct case {case_id}: data JSON must be an object")
        source_sha = hashlib.sha256(source_bytes).hexdigest()
        data_sha = hashlib.sha256(data_bytes).hexdigest()
        digest.update(case_id.encode("utf-8"))
        digest.update(str(source_path.relative_to(repo)).encode("utf-8"))
        digest.update(str(data_path.relative_to(repo)).encode("utf-8"))
        digest.update(source_sha.encode("ascii"))
        digest.update(data_sha.encode("ascii"))
        cases.append(ConstructCase(
            case_id, category, description, source_path, data_path, gate,
            expected_accept, phase, exception, points, profile, census_reason,
            numeric_policy, source_sha, data_sha))
    ids = [case.id for case in cases]
    duplicates = sorted(case_id for case_id in set(ids)
                        if ids.count(case_id) > 1)
    if duplicates:
        raise CatalogError("duplicate construct IDs: " + ", ".join(duplicates))
    return ConstructCatalog(schema, version, tuple(cases), digest.hexdigest(),
                            source, repo)
