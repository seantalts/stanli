"""Reviewed capability classification and numeric-gate policy."""

from __future__ import annotations

import dataclasses
import hashlib
import pathlib
import re
from typing import Dict, Iterable, Mapping, Optional, Sequence, Tuple

from .domains import semantic_inapplicability
from .signatures import Signature
from .status import ResultStatus
from .toml_compat import loads as toml_loads


class PolicyError(ValueError):
    pass


MATCH_FIELDS = frozenset(
    {
        "name",
        "names",
        "name_regex",
        "name_prefix",
        "name_suffix",
        "canonical_id",
        "canonical_ids",
        "contains_complex",
        "has_function_argument",
        "return_kind",
        "argument_kind",
        "min_arity",
        "max_arity",
        "probability",
    }
)


def _matches(matchers: Mapping[str, object], signature: Signature) -> bool:
    for field, expected in matchers.items():
        if field == "name" and signature.name != expected:
            return False
        if field == "names" and signature.name not in expected:
            return False
        if field == "name_regex" and not re.search(str(expected), signature.name):
            return False
        if field == "name_prefix" and not signature.name.startswith(str(expected)):
            return False
        if field == "name_suffix" and not signature.name.endswith(str(expected)):
            return False
        if field == "canonical_id" and signature.canonical_id != expected:
            return False
        if field == "canonical_ids" and signature.canonical_id not in expected:
            return False
        if field == "contains_complex" and signature.contains_complex != expected:
            return False
        if (field == "has_function_argument"
                and signature.has_function_argument != expected):
            return False
        if field == "return_kind" and signature.result.kind != expected:
            return False
        if field == "argument_kind" and not any(
                arg.kind == expected for arg in signature.arguments):
            return False
        if field == "min_arity" and signature.arity < int(expected):
            return False
        if field == "max_arity" and signature.arity > int(expected):
            return False
        if field == "probability" and signature.is_probability != expected:
            return False
    return True


def _rule_matchers(raw: Mapping[str, object]) -> Dict[str, object]:
    return {field: raw[field] for field in MATCH_FIELDS if field in raw}


def _validate_regex(value: object, rule_id: str) -> None:
    try:
        re.compile(str(value))
    except re.error as exc:
        raise PolicyError(f"policy rule {rule_id}: invalid name_regex: {exc}") \
            from exc
    if str(value).strip() in {"", ".*", "^.*$", ".+", "^.+$"}:
        raise PolicyError(
            f"policy rule {rule_id}: catch-all name_regex is forbidden")


@dataclasses.dataclass(frozen=True)
class ClassificationRule:
    id: str
    reason: str
    matchers: Mapping[str, object]
    max_matches: Optional[int] = None

    @property
    def status(self) -> ResultStatus:
        return ResultStatus.EXPECTED_UNSUPPORTED

    def matches(self, signature: Signature) -> bool:
        return _matches(self.matchers, signature)


@dataclasses.dataclass(frozen=True)
class NumericGate:
    id: str
    reason: str
    matchers: Mapping[str, object]
    max_ulp: Optional[int] = None
    abs_tol: float = 0.0
    rel_tol: float = 0.0

    def matches(self, signature: Signature) -> bool:
        return _matches(self.matchers, signature)


@dataclasses.dataclass(frozen=True)
class PolicyAudit:
    match_counts: Mapping[str, int]
    stale_rules: Tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class CapabilityPolicy:
    schema_version: int
    policy_version: str
    classification: Tuple[ClassificationRule, ...]
    numeric_gates: Tuple[NumericGate, ...]
    sha256: str
    source: str

    def classification_for(self, signature: Signature) \
            -> Optional[ClassificationRule]:
        matches = [rule for rule in self.classification
                   if rule.matches(signature)]
        if len(matches) > 1:
            names = ", ".join(rule.id for rule in matches)
            raise PolicyError(
                f"{signature.canonical_id} matches multiple capability rules: "
                f"{names}")
        return matches[0] if matches else None

    def numeric_gate_for(self, signature: Signature) -> Optional[NumericGate]:
        matches = [gate for gate in self.numeric_gates
                   if gate.matches(signature)]
        if len(matches) > 1:
            names = ", ".join(gate.id for gate in matches)
            raise PolicyError(
                f"{signature.canonical_id} matches multiple numeric gates: "
                f"{names}")
        return matches[0] if matches else None

    def numeric_gate_named(self, gate_id: str) -> NumericGate:
        matches = [gate for gate in self.numeric_gates if gate.id == gate_id]
        if len(matches) != 1:
            raise PolicyError(
                f"construct references unknown numeric gate {gate_id!r}")
        return matches[0]

    def audit(self, signatures: Iterable[Signature]) -> PolicyAudit:
        counts = {rule.id: 0 for rule in self.classification}
        for signature in signatures:
            # Structural inapplicability wins before capability policy in the
            # runner, so the audit must count exactly that same reachable set.
            if (signature.structural_inapplicability() is not None
                    or semantic_inapplicability(signature) is not None):
                continue
            rule = self.classification_for(signature)
            if rule is not None:
                counts[rule.id] += 1
        for rule in self.classification:
            if rule.max_matches is not None and counts[rule.id] > rule.max_matches:
                raise PolicyError(
                    f"policy rule {rule.id} matched {counts[rule.id]} signatures; "
                    f"reviewed maximum is {rule.max_matches}")
        stale = tuple(sorted(rule_id for rule_id, count in counts.items()
                             if count == 0))
        return PolicyAudit(counts, stale)

    def to_metadata(self) -> Dict[str, object]:
        return {
            "schema_version": self.schema_version,
            "policy_version": self.policy_version,
            "sha256": self.sha256,
            "classification_rules": [rule.id for rule in self.classification],
            "numeric_gates": [gate.id for gate in self.numeric_gates],
        }


def _known_fields(raw: Mapping[str, object], permitted: Sequence[str],
                  label: str) -> None:
    unknown = sorted(set(raw) - set(permitted))
    if unknown:
        raise PolicyError(f"{label}: unknown fields: {', '.join(unknown)}")


def _validate_matchers(rule_id: str, matchers: Mapping[str, object],
                       max_matches: Optional[int], classification: bool) -> None:
    if not matchers:
        raise PolicyError(f"policy rule {rule_id}: at least one matcher is required")
    if "name_regex" in matchers:
        _validate_regex(matchers["name_regex"], rule_id)
    for field in ("contains_complex", "has_function_argument", "probability"):
        if field in matchers and not isinstance(matchers[field], bool):
            raise PolicyError(f"policy rule {rule_id}: {field} must be boolean")
    for field in ("name", "name_regex", "name_prefix", "name_suffix",
                  "canonical_id", "return_kind", "argument_kind"):
        if field in matchers and not isinstance(matchers[field], str):
            raise PolicyError(f"policy rule {rule_id}: {field} must be a string")
    for field in ("names", "canonical_ids"):
        value = matchers.get(field)
        if value is not None and (
                not isinstance(value, list)
                or not all(isinstance(item, str) for item in value)):
            raise PolicyError(
                f"policy rule {rule_id}: {field} must be an array of strings")
    for field in ("min_arity", "max_arity"):
        value = matchers.get(field)
        if value is not None and (not isinstance(value, int)
                                  or isinstance(value, bool) or value < 0):
            raise PolicyError(
                f"policy rule {rule_id}: {field} must be a nonnegative integer")

    # Product-boundary matchers are structurally narrow.  A name-based
    # exception can still be legitimate, but it must carry a small reviewed
    # ceiling so `.*` or a broadened family cannot absorb the inventory.
    structural_boundary = (
        matchers.get("contains_complex") is True
        or matchers.get("return_kind") == "tuple"
        or matchers.get("has_function_argument") is True
    )
    # A broad structural exception may not be weakened by unrelated boolean
    # matchers.  `contains_complex = false` plus `probability = true`, for
    # example, is still an attempt to swallow almost every ordinary density.
    broad_fields = {"contains_complex", "return_kind", "has_function_argument"}
    has_broad_matchers = any(field in matchers for field in broad_fields)
    exact = any(field in matchers
                for field in ("name", "names", "canonical_id", "canonical_ids"))
    named_family = any(field in matchers
                       for field in ("name_regex", "name_prefix", "name_suffix"))
    if classification and not structural_boundary:
        if has_broad_matchers:
            raise PolicyError(
                f"policy rule {rule_id}: negated structural matchers require "
                "an exact name or named family")
        if not (exact or named_family):
            raise PolicyError(
                f"policy rule {rule_id}: unsupported classification is too broad")
        if max_matches is None or not 1 <= max_matches <= 512:
            raise PolicyError(
                f"policy rule {rule_id}: named exceptions require "
                "max_matches between 1 and 512")


def _classification_rule(raw: Mapping[str, object]) -> ClassificationRule:
    permitted = tuple(MATCH_FIELDS) + ("id", "reason", "status", "max_matches")
    rule_id = str(raw.get("id", ""))
    _known_fields(raw, permitted, f"policy rule {rule_id or '(unnamed)'}")
    reason = str(raw.get("reason", "")).strip()
    if not rule_id or not reason:
        raise PolicyError("classification rules require nonempty id and reason")
    status = str(raw.get("status", ResultStatus.EXPECTED_UNSUPPORTED.value))
    if status != ResultStatus.EXPECTED_UNSUPPORTED.value:
        raise PolicyError(
            f"policy rule {rule_id}: capability policy may only classify "
            "expected_unsupported")
    max_matches = (int(raw["max_matches"])
                   if raw.get("max_matches") is not None else None)
    matchers = _rule_matchers(raw)
    _validate_matchers(rule_id, matchers, max_matches, True)
    return ClassificationRule(rule_id, reason, matchers, max_matches)


def _numeric_gate(raw: Mapping[str, object]) -> NumericGate:
    permitted = tuple(MATCH_FIELDS) + (
        "id", "reason", "max_ulp", "abs_tol", "rel_tol")
    gate_id = str(raw.get("id", ""))
    _known_fields(raw, permitted, f"numeric gate {gate_id or '(unnamed)'}")
    reason = str(raw.get("reason", "")).strip()
    if not gate_id or not reason:
        raise PolicyError("numeric gates require nonempty id and reason")
    matchers = _rule_matchers(raw)
    _validate_matchers(gate_id, matchers, None, False)
    max_ulp = int(raw["max_ulp"]) if raw.get("max_ulp") is not None else None
    abs_tol = float(raw.get("abs_tol", 0.0))
    rel_tol = float(raw.get("rel_tol", 0.0))
    if max_ulp is not None and max_ulp < 0:
        raise PolicyError(f"numeric gate {gate_id}: max_ulp must be nonnegative")
    if abs_tol < 0 or rel_tol < 0:
        raise PolicyError(f"numeric gate {gate_id}: tolerances must be nonnegative")
    return NumericGate(gate_id, reason, matchers, max_ulp, abs_tol, rel_tol)


def load_policy(path: pathlib.Path) -> CapabilityPolicy:
    path = pathlib.Path(path)
    source = path.read_text(encoding="utf-8")
    try:
        raw = toml_loads(source)
    except ValueError as exc:
        raise PolicyError(f"could not parse {path}: {exc}") from exc
    _known_fields(raw, ("schema_version", "policy_version", "classification",
                        "numeric_gate"), "policy")
    schema_version = int(raw.get("schema_version", 0))
    if schema_version != 1:
        raise PolicyError(f"unsupported policy schema_version {schema_version}")
    policy_version = str(raw.get("policy_version", "")).strip()
    if not policy_version:
        raise PolicyError("policy_version is required")
    classification = tuple(
        _classification_rule(item)
        for item in list(raw.get("classification", []))
    )
    gates = tuple(_numeric_gate(item)
                  for item in list(raw.get("numeric_gate", [])))
    ids = [rule.id for rule in classification] + [gate.id for gate in gates]
    duplicate_ids = sorted({item for item in ids if ids.count(item) > 1})
    if duplicate_ids:
        raise PolicyError("duplicate policy IDs: " + ", ".join(duplicate_ids))
    digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    return CapabilityPolicy(schema_version, policy_version, classification,
                            gates, digest, source)
