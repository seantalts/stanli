"""Inventory selection and honest Phase-1 classification runner."""

from __future__ import annotations

import dataclasses
import hashlib
import pathlib
import subprocess
import tempfile
import time
from typing import Dict, Optional

from .catalog import ConstructCase, ConstructCatalog
from .domains import semantic_inapplicability
from .policy import CapabilityPolicy
from .report import ConformanceReport, Scope, utc_timestamp
from .signatures import Inventory, Signature
from .status import CaseResult, ResultStatus


GENERATOR_VERSION = "inventory-v2"

INAPPLICABLE_REASONS = {
    "rng_only": "RNG-only signatures do not define a deterministic gradient.",
    "no_real_bearing_result":
        "The result has no real-bearing lane that can contribute to target.",
    "no_differentiable_path":
        "No argument can be sourced from an unconstrained real parameter.",
}


@dataclasses.dataclass(frozen=True)
class Selection:
    case: Optional[str] = None
    filter: Optional[str] = None
    shard_index: Optional[int] = None
    shard_count: Optional[int] = None

    def __post_init__(self) -> None:
        if (self.shard_index is None) != (self.shard_count is None):
            raise ValueError("shard_index and shard_count must be provided together")
        if self.shard_count is not None:
            if self.shard_count < 1:
                raise ValueError("shard count must be positive")
            if not 1 <= self.shard_index <= self.shard_count:
                raise ValueError("shard index is one-based and must be within count")

    @property
    def complete(self) -> bool:
        return (self.case is None and not self.filter
                and self.shard_index is None)

    @property
    def shard(self) -> Optional[str]:
        if self.shard_index is None:
            return None
        return f"{self.shard_index}/{self.shard_count}"

    def matches(self, signature: Signature) -> bool:
        if self.case is not None:
            wanted = self.case
            if wanted.startswith("signature:"):
                wanted = wanted[len("signature:"):]
            if signature.canonical_id != wanted and signature.case_id != self.case:
                return False
        if self.filter:
            needle = self.filter.lower()
            haystacks = (signature.case_id, signature.raw, signature.name)
            if not any(needle in text.lower() for text in haystacks):
                return False
        if self.shard_index is not None:
            if partition_for(signature.case_id, self.shard_count) \
                    != self.shard_index:
                return False
        return True

    def matches_construct(self, case: ConstructCase) -> bool:
        if self.case is not None and self.case not in (case.id, case.case_id):
            return False
        if self.filter:
            needle = self.filter.lower()
            haystacks = (case.case_id, case.id, case.category,
                         case.description, str(case.source))
            if not any(needle in text.lower() for text in haystacks):
                return False
        if self.shard_index is not None:
            if partition_for(case.case_id, self.shard_count) \
                    != self.shard_index:
                return False
        return True


def partition_for(case_id: str, count: int) -> int:
    """Stable one-based partition independent of dump ordering."""
    if count < 1:
        raise ValueError("partition count must be positive")
    digest = hashlib.sha256(case_id.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big") % count + 1


def partition_manifest(inventory: Inventory, count: int,
                       catalog: Optional[ConstructCatalog] = None) \
        -> Dict[str, object]:
    all_case_ids = [signature.case_id for signature in inventory.signatures]
    if catalog is not None:
        all_case_ids.extend(case.case_id for case in catalog.cases)
    partitions = []
    for index in range(1, count + 1):
        case_ids = sorted(case_id for case_id in all_case_ids
                          if partition_for(case_id, count) == index)
        partitions.append({"shard": f"{index}/{count}",
                           "case_ids": case_ids,
                           "count": len(case_ids)})
    return {
        "schema_version": 1,
        "inventory_sha256": inventory.raw_sha256,
        "stanc_build_id": inventory.stanc_build_id,
        "stanc_sha256": inventory.stanc_sha256,
        "total_cases": len(all_case_ids),
        "total_names": inventory.name_count,
        "construct_catalog_sha256": catalog.sha256 if catalog else None,
        "total_constructs": catalog.count if catalog else 0,
        "partitions": partitions,
    }


def _repro(signature: Signature, stanc: pathlib.Path,
           cmdstan: Optional[pathlib.Path], build: pathlib.Path) -> str:
    command = ["python3", "harnesses/stan_conformance.py", "--stanc",
               str(stanc), "--build", str(build), "--case",
               signature.canonical_id]
    if cmdstan is not None:
        command[4:4] = ["--cmdstan", str(cmdstan), "--mode", "scalar"]
    # Canonical IDs contain callback parentheses and arrows.  One quoted
    # argument is copy/paste-safe in POSIX shells and readable in reports.
    return " ".join(("'" + item.replace("'", "'\\''") + "'"
                     if any(char in item for char in " ()[]=><,") else item)
                    for item in command)


def classify(signature: Signature, policy: CapabilityPolicy,
             stanc: pathlib.Path, cmdstan: Optional[pathlib.Path],
             build: pathlib.Path) -> CaseResult:
    started = time.monotonic()
    structural = signature.structural_inapplicability()
    details: Dict[str, object] = {"signature": signature.to_dict()}
    gate = policy.numeric_gate_for(signature)
    if gate is not None:
        details["numeric_gate"] = {
            "id": gate.id,
            "reason": gate.reason,
            "max_ulp": gate.max_ulp,
            "abs_tol": gate.abs_tol,
            "rel_tol": gate.rel_tol,
        }
    if structural is not None:
        return CaseResult(
            case_id=signature.case_id,
            inventory_id=signature.canonical_id,
            family=signature.name,
            kind="signature",
            status=ResultStatus.INAPPLICABLE,
            reason=INAPPLICABLE_REASONS[structural],
            details={**details, "classification": structural},
            timings={"classification_seconds": time.monotonic() - started},
        )

    semantic = semantic_inapplicability(signature)
    if semantic is not None:
        return CaseResult(
            case_id=signature.case_id,
            inventory_id=signature.canonical_id,
            family=signature.name,
            kind="signature",
            status=ResultStatus.INAPPLICABLE,
            reason=semantic,
            details={**details, "classification": "semantic_no_gradient"},
            timings={"classification_seconds": time.monotonic() - started},
        )

    capability = policy.classification_for(signature)
    if capability is not None:
        return CaseResult(
            case_id=signature.case_id,
            inventory_id=signature.canonical_id,
            family=signature.name,
            kind="signature",
            status=ResultStatus.EXPECTED_UNSUPPORTED,
            reason=capability.reason,
            policy_rule=capability.id,
            policy_reason=capability.reason,
            repro_command=_repro(signature, stanc, cmdstan, build),
            details=details,
            timings={"classification_seconds": time.monotonic() - started},
        )

    # Phase 1 stops here on purpose.  A parser success is not runtime
    # evidence, and the absence of a generator is a blocking harness task.
    return CaseResult(
        case_id=signature.case_id,
        inventory_id=signature.canonical_id,
        family=signature.name,
        kind="signature",
        status=ResultStatus.GENERATOR_GAP,
        reason="No type-directed conformance generator is registered yet.",
        repro_command=_repro(signature, stanc, cmdstan, build),
        details=details,
        timings={"classification_seconds": time.monotonic() - started},
    )


def classify_construct(case: ConstructCase, stanc: pathlib.Path,
                       cmdstan: Optional[pathlib.Path], build: pathlib.Path,
                       repo: pathlib.Path) -> CaseResult:
    command = ["python3", "harnesses/stan_conformance.py", "--stanc",
               str(stanc), "--build", str(build), "--case", case.case_id]
    if cmdstan is not None:
        command[4:4] = ["--cmdstan", str(cmdstan), "--mode", "scalar"]
    repro = " ".join(("'" + item.replace("'", "'\\''") + "'"
                      if any(char in item for char in " ()[]=><,") else item)
                     for item in command)
    details = {"construct": case.to_dict(repo)}
    if case.oracle_gate == "census":
        started = time.monotonic()
        try:
            with tempfile.TemporaryDirectory(
                    prefix="stanli-conformance-census-") as raw:
                output = pathlib.Path(raw) / "census.hpp"
                compiled = subprocess.run(
                    [str(stanc), str(case.source), f"--o={output}"],
                    capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as exc:
            return CaseResult(
                case_id=case.case_id, inventory_id=case.inventory_id,
                family=case.category, kind="construct",
                status=ResultStatus.HARNESS_ERROR,
                reason=f"Could not execute the stanc3 source census: {exc}",
                repro_command=repro, probe_attempted=True, details=details,
                timings={"classification_seconds":
                         time.monotonic() - started})
        if compiled.returncode != 0:
            return CaseResult(
                case_id=case.case_id, inventory_id=case.inventory_id,
                family=case.category, kind="construct",
                status=ResultStatus.GENERATOR_GAP,
                reason="A cataloged stanc3 census source no longer compiles.",
                repro_command=repro, probe_attempted=True,
                details={**details,
                         "stanc_stdout": compiled.stdout[-4000:],
                         "stanc_stderr": compiled.stderr[-4000:]},
                timings={"classification_seconds":
                         time.monotonic() - started})
        return CaseResult(
            case_id=case.case_id, inventory_id=case.inventory_id,
            family=case.category, kind="construct",
            status=ResultStatus.INAPPLICABLE,
            reason=case.census_reason or "Compilation-only source census.",
            probe_attempted=True, details=details,
            timings={"classification_seconds": time.monotonic() - started})
    return CaseResult(
        case_id=case.case_id,
        inventory_id=case.inventory_id,
        family=case.category,
        kind="construct",
        status=ResultStatus.GENERATOR_GAP,
        reason="Construct evaluation requires the pinned CmdStan differential phase.",
        repro_command=repro,
        details=details,
    )


def _git_commit(path: Optional[pathlib.Path]) -> Optional[str]:
    if path is None:
        return None
    try:
        result = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def run_inventory(inventory: Inventory, policy: CapabilityPolicy,
                  selection: Selection, stanc: pathlib.Path,
                  cmdstan: Optional[pathlib.Path], build: pathlib.Path,
                  repo: pathlib.Path, created_at: Optional[str] = None,
                  jobs: int = 1,
                  catalog: Optional[ConstructCatalog] = None) \
        -> ConformanceReport:
    if jobs < 1:
        raise ValueError("jobs must be positive")
    audit = policy.audit(inventory.signatures)
    selected = tuple(signature for signature in inventory.signatures
                     if selection.matches(signature))
    selected_constructs = (() if catalog is None else tuple(
        case for case in catalog.cases if selection.matches_construct(case)))
    results = tuple(sorted(
        [classify(signature, policy, stanc, cmdstan, build)
         for signature in selected]
        + [classify_construct(case, stanc, cmdstan, build, repo)
           for case in selected_constructs], key=lambda result: result.case_id))
    total_cases = inventory.signature_count + (catalog.count if catalog else 0)
    scope = Scope(
        complete=selection.complete,
        selected_cases=len(results),
        total_cases=total_cases,
        case=selection.case,
        filter=selection.filter or None,
        shard=selection.shard,
    )
    tools: Dict[str, object] = {
        "generator_version": GENERATOR_VERSION,
        "repository_commit": _git_commit(repo),
        "stanc": str(stanc),
        "build": str(build),
        "jobs": jobs,
    }
    if cmdstan is not None:
        tools["cmdstan"] = str(cmdstan)
        tools["cmdstan_commit"] = _git_commit(cmdstan)
    inventory_metadata = inventory.to_metadata()
    inventory_metadata["total_items"] = total_cases
    inventory_metadata["total_constructs"] = catalog.count if catalog else 0
    if catalog is not None:
        inventory_metadata["construct_catalog"] = catalog.to_metadata()
    return ConformanceReport(
        created_at=created_at or utc_timestamp(),
        inventory=inventory_metadata,
        policy=policy.to_metadata(),
        scope=scope,
        tools=tools,
        results=results,
        policy_stale_rules=audit.stale_rules,
    )
