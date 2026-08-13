"""Compile and evaluate generated scalar and probability conformance."""

from __future__ import annotations

import concurrent.futures
import dataclasses
import hashlib
import pathlib
import sys
from typing import Mapping, Optional, Sequence, Tuple

from .compare import Gate
from .evaluate import evaluate_generated_case
from .generate import (GeneratedCase, GeneratedShard, VectorizedCaseSpec,
                       generated_inventory, make_scalar_shards,
                       make_subshard_source)
from .oracle import (OracleError, ReferenceBuild, ToolchainVersions,
                     cached_reference_build, prepare_reference_runtime,
                     validate_toolchain)
from .policy import CapabilityPolicy, ClassificationRule
from .protocol import JsonLinesClient, ProtocolError
from .report import ConformanceReport
from .signatures import Inventory
from .status import CaseResult, ResultStatus
from .transport import (TransportError, transport_identity,
                        worker_environment)


SCALAR_GENERATOR_VERSION = "typed-shards-v4-bridgestan"


def _failure(case, status: ResultStatus, reason: str,
             details: Mapping[str, object], repro: Optional[str],
             policy_rule: Optional[ClassificationRule] = None,
             probe_attempted: bool = False) -> CaseResult:
    return CaseResult(
        case_id=case.case_id,
        inventory_id=case.inventory_id,
        family=case.spec.signature.name,
        kind="signature",
        status=status,
        reason=reason,
        policy_rule=policy_rule.id if policy_rule else None,
        policy_reason=policy_rule.reason if policy_rule else None,
        repro_command=repro,
        probe_attempted=probe_attempted,
        details=dict(details),
    )


def _gate(policy: CapabilityPolicy, case) -> Gate:
    numeric = policy.numeric_gate_for(case.spec.signature)
    if numeric is None:
        return Gate(max_ulp=0)
    return Gate(max_ulp=numeric.max_ulp, abs_tol=numeric.abs_tol,
                rel_tol=numeric.rel_tol)


def _repro(case, stanc: pathlib.Path, cmdstan: pathlib.Path,
           build: pathlib.Path) -> str:
    values = ["python3", "harnesses/stan_conformance.py", "--stanc",
              str(stanc), "--cmdstan", str(cmdstan), "--build", str(build),
              "--mode", "scalar", "--case", case.inventory_id]
    return " ".join(("'" + value.replace("'", "'\\''") + "'"
                     if any(char in value for char in " ()[]=><,") else value)
                    for value in values)


def _build_one(shard: GeneratedShard, cmdstan: pathlib.Path,
               stanc: pathlib.Path, cache_root: pathlib.Path,
               deps: pathlib.Path, versions: ToolchainVersions,
               policy: CapabilityPolicy) \
        -> ReferenceBuild:
    return cached_reference_build(
        shard.source.encode("utf-8"), f"{shard.id}.stan", cache_root,
        "signatures",
        {"generator_version": SCALAR_GENERATOR_VERSION,
         "policy_sha256": policy.sha256},
        cmdstan, stanc, deps, versions)


class _ActiveCaseClient:
    def __init__(self, client: JsonLinesClient, active_cases: Mapping[int, int]):
        self.client = client
        self.active_cases = active_cases

    def request(self, payload: Mapping[str, object]):
        request = dict(payload)
        active = request.get("active_case")
        if isinstance(active, int):
            request["active_case"] = self.active_cases[active]
        return self.client.request(request)


def _retry_source(reference: ReferenceBuild, cases: Sequence[GeneratedCase],
                  parameter_count: int) -> Tuple[pathlib.Path, str]:
    source = make_subshard_source(cases, parameter_count)
    digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    directory = reference.source.parent / "stanli-subshards"
    path = directory / f"retry-{digest[:16]}.stan"
    directory.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != source:
        path.write_text(source, encoding="utf-8")
    return path, digest


def _evaluate_shard(shard: GeneratedShard, reference: ReferenceBuild,
                    policy: CapabilityPolicy, stanc: pathlib.Path,
                    cmdstan: pathlib.Path, build: pathlib.Path,
                    python_executable: pathlib.Path,
                    pythonpath: Optional[pathlib.Path], timeout: float,
                    worker_path: Optional[pathlib.Path] = None) \
        -> Tuple[CaseResult, ...]:
    repros = {case.case_id: _repro(case, stanc, cmdstan, build)
              for case in shard.cases}
    if reference.executable is None:
        status = (ResultStatus.GENERATOR_GAP
                  if reference.error and reference.error.startswith("stanc_fail")
                  else ResultStatus.HARNESS_ERROR)
        reason = ("stanc rejected the generated scalar shard."
                  if status == ResultStatus.GENERATOR_GAP
                  else "Reference shard compilation failed.")
        return tuple(_failure(
            case, status, reason,
            {"shard": shard.result_metadata(),
             "reference_error": reference.error},
            repros[case.case_id]) for case in shard.cases)

    worker = worker_path or pathlib.Path(__file__).with_name("model_worker.py")
    environment = worker_environment(pythonpath)
    reference_command = [str(python_executable), "-u", str(worker),
                         str(reference.executable), "--backend", "reference"]
    try:
        with JsonLinesClient(reference_command, timeout=timeout,
                             env=environment, label="reference") as ref:
            def evaluate_group(cases: Tuple[GeneratedCase, ...],
                               source: pathlib.Path, source_sha256: str,
                               retry: bool) -> Tuple[CaseResult, ...]:
                command = [str(python_executable), "-u", str(worker),
                           str(source), "--backend", "stanli"]
                active_cases = {
                    case.active_case: (index if retry else case.active_case)
                    for index, case in enumerate(cases, 1)
                }
                should_split = False
                with JsonLinesClient(command, timeout=timeout,
                                     env=environment,
                                     label="stanli") as raw_ours:
                    try:
                        description = raw_ours.request({
                            "action": "describe",
                            "active_case": next(iter(active_cases.values())),
                        })
                        if description.get("protocol_error"):
                            raise RuntimeError(str(description.get(
                                "message", "stanli protocol error")))
                        if "accepted" not in description:
                            raise ProtocolError(
                                "stanli description omitted acceptance status")
                        stanli_failure = None
                    except ProtocolError as exc:
                        stanli_failure = str(exc)
                        description = {"accepted": False}
                    if not description["accepted"] and len(cases) > 1:
                        should_split = True
                    elif stanli_failure is not None:
                        case = cases[0]
                        reference_description = ref.request({
                            "action": "describe",
                            "active_case": case.active_case,
                        })
                        if reference_description.get("protocol_error"):
                            raise RuntimeError(str(reference_description.get(
                                "message", "reference protocol error")))
                        if "accepted" not in reference_description:
                            raise ProtocolError(
                                "reference description omitted acceptance "
                                "status")
                        if not reference_description["accepted"]:
                            return (_failure(
                                case, ResultStatus.GENERATOR_GAP,
                                "The reference rejected the isolated case "
                                "while stanli's transport failed.",
                                {"shard": shard.result_metadata(),
                                 "source_path": str(reference.source),
                                 "reference_description":
                                     reference_description,
                                 "stanli_transport_error": stanli_failure},
                                repros[case.case_id],
                                probe_attempted=True),)
                        policy_rule = policy.classification_for(
                            case.spec.signature)
                        status = (ResultStatus.EXPECTED_UNSUPPORTED
                                  if policy_rule is not None else
                                  ResultStatus.UNEXPECTED_UNSUPPORTED)
                        reason = (policy_rule.reason if policy_rule is not None
                                  else "stanli failed while loading the "
                                  "isolated case: " + stanli_failure)
                        return (_failure(
                            case, status, reason,
                            {"shard": shard.result_metadata(),
                             "source_path": str(reference.source),
                             "reference_description": reference_description,
                             "stanli_shard": {
                                 "case_count": 1,
                                 "source_path": str(source),
                                 "source_sha256": source_sha256,
                                 "retry": retry,
                             }},
                            repros[case.case_id], policy_rule,
                            probe_attempted=True),)
                    else:
                        ours = _ActiveCaseClient(raw_ours, active_cases)
                        results = []
                        for case in cases:
                            policy_rule = policy.classification_for(
                                case.spec.signature)
                            result = evaluate_generated_case(
                                case, ref, ours, _gate(policy, case),
                                policy_rule, repros[case.case_id])
                            result = dataclasses.replace(
                                result,
                                details={
                                    **dict(result.details),
                                    "shard": shard.result_metadata(),
                                    "source_path": str(reference.source),
                                    "stanli_shard": {
                                        "case_count": len(cases),
                                        "source_path": str(source),
                                        "source_sha256": source_sha256,
                                        "retry": retry,
                                    },
                                    "reference_cache_hit": reference.cache_hit,
                                    "reference_build_seconds":
                                        reference.build_seconds,
                                })
                            results.append(result)
                        return tuple(results)
                if should_split:
                    midpoint = len(cases) // 2
                    groups = (cases[:midpoint], cases[midpoint:])
                    results = []
                    for group in groups:
                        path, digest = _retry_source(
                            reference, group, shard.parameter_count)
                        results.extend(evaluate_group(
                            group, path, digest, True))
                    return tuple(results)
                raise ProtocolError("stanli retry shard made no progress")

            return evaluate_group(
                shard.cases, reference.source, reference.source_sha256, False)
    except (OSError, ProtocolError, RuntimeError) as exc:
        return tuple(_failure(
            case, ResultStatus.HARNESS_ERROR,
            f"Could not run paired shard processes: {exc}",
            {"shard": shard.result_metadata()}, repros[case.case_id])
                     for case in shard.cases)


def run_scalar_phase(report: ConformanceReport, inventory: Inventory,
                     policy: CapabilityPolicy, cmdstan: pathlib.Path,
                     stanc: pathlib.Path, build: pathlib.Path,
                     deps: pathlib.Path, cache_root: pathlib.Path,
                     python_executable: pathlib.Path = pathlib.Path(sys.executable),
                     pythonpath: Optional[pathlib.Path] = None,
                     shard_size: int = 128, jobs: int = 1,
                     timeout: float = 60.0) -> ConformanceReport:
    """Replace generator-gap rows for the selected scalar subset."""
    if jobs < 1:
        raise ValueError("jobs must be positive")
    selected_ids = {result.inventory_id for result in report.results}
    specs = tuple(spec for spec in generated_inventory(inventory)
                  if spec.inventory_id in selected_ids)
    if not specs:
        return report
    shards = make_scalar_shards(specs, shard_size)

    try:
        versions = validate_toolchain(cmdstan, stanc, deps)
        identity = transport_identity(
            python_executable, pythonpath, versions.bridgestan_version)
        prepare_reference_runtime(cmdstan, stanc, deps, versions)
    except (OracleError, TransportError) as exc:
        replacements = {}
        for shard in shards:
            for case in shard.cases:
                replacements[case.case_id] = _failure(
                    case, ResultStatus.HARNESS_ERROR,
                    f"Conformance runtime validation failed: {exc}",
                    {"phase": "runtime_validation"},
                    _repro(case, stanc, cmdstan, build))
        return _replace(report, replacements,
                        {"scalar_generator_version": SCALAR_GENERATOR_VERSION,
                         "runtime_error": str(exc)})

    cache_root.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        builds = tuple(pool.map(
            lambda shard: _build_one(shard, cmdstan, stanc, cache_root,
                                     deps, versions, policy), shards))
    def evaluate_one(pair):
        shard, reference = pair
        return _evaluate_shard(
            shard, reference, policy, stanc, cmdstan, build,
            python_executable, pythonpath, timeout)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        evaluated = tuple(pool.map(evaluate_one, zip(shards, builds)))
    replacements = {}
    for shard_results in evaluated:
        for result in shard_results:
            replacements[result.case_id] = result
    retry_shards = {
        str(result.details["stanli_shard"]["source_sha256"])
        for result in replacements.values()
        if isinstance(result.details.get("stanli_shard"), dict)
        and result.details["stanli_shard"].get("retry")
    }
    return _replace(
        report, replacements,
        {"scalar_generator_version": SCALAR_GENERATOR_VERSION,
         "scalar_shards": len(shards), "generated_cases": len(specs),
         "vectorized_cases": sum(isinstance(spec, VectorizedCaseSpec)
                                 for spec in specs),
         "probability_cases": sum(spec.signature.is_probability
                                  for spec in specs),
         "reference_cache_hits": sum(build.cache_hit for build in builds),
         "reference_build_seconds": sum(build.build_seconds
                                        for build in builds),
         "reference_toolchain": versions.to_dict(),
         "reference_backend": "BridgeStan",
         "stanli_retry_cases": sum(
             isinstance(result.details.get("stanli_shard"), dict)
             and bool(result.details["stanli_shard"].get("retry"))
             for result in replacements.values()),
         "stanli_retry_shards": len(retry_shards),
         "stanli_transport_identity": identity})


def _replace(report: ConformanceReport, replacements: Mapping[str, CaseResult],
             tool_details: Mapping[str, object]) -> ConformanceReport:
    results = tuple(sorted(
        (replacements.get(result.case_id, result) for result in report.results),
        key=lambda result: result.case_id))
    tools = {**dict(report.tools), **dict(tool_details)}
    return dataclasses.replace(report, results=results, tools=tools)
