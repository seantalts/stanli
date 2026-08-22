"""Differential evaluation for the named language-construct catalog."""

from __future__ import annotations

import concurrent.futures
import dataclasses
import math
import pathlib
import sys
import time
from typing import Dict, Mapping, Optional, Tuple

from .catalog import ConstructCase, ConstructCatalog
from .compare import (DEFAULT_GATE, Expectation, Gate, Observation,
                      OutcomeComparison,
                      compare_observations)
from .oracle import (OracleError, ReferenceBuild, ToolchainVersions,
                     cached_reference_build, prepare_reference_runtime,
                     validate_toolchain)
from .policy import CapabilityPolicy
from .protocol import JsonLinesClient, ProtocolError
from .report import ConformanceReport
from .status import CaseResult, ResultStatus
from .transport import (TransportError, transport_identity,
                        worker_environment)


CONSTRUCT_GENERATOR_VERSION = "construct-catalog-v2-bridgestan"

_PRECEDENCE = {
    ResultStatus.HARNESS_ERROR: 0,
    ResultStatus.CRASHED: 1,
    ResultStatus.GENERATOR_GAP: 2,
    ResultStatus.MISMATCH: 3,
    ResultStatus.UNEXPECTED_UNSUPPORTED: 4,
    ResultStatus.VERIFIED: 5,
}


def _build_one(case: ConstructCase, cmdstan: pathlib.Path,
               stanc: pathlib.Path, cache_root: pathlib.Path,
               deps: pathlib.Path, versions: ToolchainVersions) \
        -> ReferenceBuild:
    return cached_reference_build(
        case.source.read_bytes(),
        "construct-" + case.source_sha256[:16] + ".stan",
        cache_root, "constructs",
        {"generator_version": CONSTRUCT_GENERATOR_VERSION},
        cmdstan, stanc, deps, versions)


def _profile_points(profile: Optional[str], width: int) \
        -> Tuple[Tuple[float, ...], ...]:
    if profile in (None, "not-evaluated"):
        return ()
    if profile != "zeros-and-asymmetric":
        raise ValueError(f"unknown construct point profile {profile!r}")
    zero = (0.0,) * width
    first = tuple(0.02 * ((index % 5) - 2) for index in range(width))
    second = tuple(0.1 + 0.05 * (index % 7) - 0.15 * (index % 3)
                   for index in range(width))
    return (zero, first, second)


def _observation(response: Mapping[str, object]) -> Observation:
    if response.get("protocol_error"):
        raise ProtocolError(str(response.get("message", "protocol error")))
    return Observation.from_dict(response)


def _numeric_gate(case: ConstructCase, policy: CapabilityPolicy) -> Gate:
    if case.numeric_policy is None:
        return DEFAULT_GATE
    configured = policy.numeric_gate_named(case.numeric_policy)
    return Gate(max_ulp=configured.max_ulp, abs_tol=configured.abs_tol,
                rel_tol=configured.rel_tol)


def _construction_outcome(case: ConstructCase,
                          reference: Mapping[str, object],
                          stanli: Mapping[str, object]) \
        -> Optional[OutcomeComparison]:
    reference_accepted = bool(reference.get("accepted"))
    stanli_accepted = bool(stanli.get("accepted"))
    common = {"reference_description": dict(reference),
              "stanli_description": dict(stanli)}

    if not reference_accepted:
        comparison = compare_observations(
            _observation(stanli), _observation(reference),
            Expectation(should_accept=case.expected_accept,
                        phase=case.expected_phase,
                        exception_category=case.expected_exception), Gate())
        return dataclasses.replace(
            comparison, details={**common, **dict(comparison.details)})

    if not stanli_accepted:
        # A construction-time refusal is a coverage gap whatever the catalog
        # expected of the accepted case. stanli said no, loudly, at compile
        # time -- it did not compute a different answer, and a caller cannot
        # mistake it for one. That is the same kind of finding as a function
        # that is not wired up, so it is reported and not gated. A genuine
        # disagreement about a value still reaches MISMATCH below, through
        # the evaluation comparison, which is where a wrong answer lives.
        reason = ("stanli rejected during construction instead of the "
                  f"cataloged {case.expected_phase} phase."
                  if not case.expected_accept else
                  "The reference constructed the case but stanli refused it: "
                  + str(stanli.get("message", "unknown rejection")))
        return OutcomeComparison(
            ResultStatus.UNEXPECTED_UNSUPPORTED, reason, common)

    if not case.expected_accept and case.expected_phase == "construction":
        return OutcomeComparison(
            ResultStatus.GENERATOR_GAP,
            "The reference accepted a cataloged construction rejection.",
            common)

    reference_count = int(reference.get("parameter_count", -1))
    stanli_count = int(stanli.get("parameter_count", -1))
    if reference_count != stanli_count:
        return OutcomeComparison(
            ResultStatus.MISMATCH,
            f"Unconstrained parameter count differs: stanli {stanli_count}, "
            f"reference {reference_count}.", common)
    reference_names = tuple(str(x) for x in reference.get("parameter_names", []))
    stanli_names = tuple(str(x) for x in stanli.get("parameter_names", []))
    if reference_names != stanli_names:
        return OutcomeComparison(
            ResultStatus.MISMATCH,
            "Unconstrained parameter names or order disagree.", common)
    return None


def _stanli_transport_outcome(case: ConstructCase,
                              reference: Mapping[str, object],
                              phase: str, error: ProtocolError) \
        -> OutcomeComparison:
    # The worker stopped answering: it died, timed out, or spoke nonsense.
    # None of those is a refusal, so this does not go through
    # compare_observations -- a refusal carries a message stanli chose to
    # send, and reading a dead process as one that declined the case is
    # what let a SIGSEGV here count as a coverage gap (status.py).
    del case
    message = f"stanli transport failed during {phase}: {error}"
    return OutcomeComparison(
        ResultStatus.CRASHED, message,
        {"stanli": Observation(accepted=False, message=message).to_dict(),
         "reference": _observation(reference).to_dict(),
         "transport_phase": phase})


def _result(case: ConstructCase, status: ResultStatus, reason: str,
            details: Mapping[str, object], repro: Optional[str],
            started: float) -> CaseResult:
    return CaseResult(
        case_id=case.case_id,
        inventory_id=case.inventory_id,
        family=case.category,
        kind="construct",
        status=status,
        reason=reason,
        repro_command=repro,
        probe_attempted=True,
        details=dict(details),
        timings={"evaluation_seconds": time.monotonic() - started},
    )


def _evaluate_case(case: ConstructCase, reference: ReferenceBuild,
                   python_executable: pathlib.Path,
                   pythonpath: Optional[pathlib.Path], timeout: float,
                   repo: pathlib.Path,
                   repro: Optional[str], gate: Gate) -> CaseResult:
    started = time.monotonic()
    base_details = {
        "construct": case.to_dict(repo),
        "source_path": str(reference.source),
        "data_path": str(case.data),
        "reference_cache_hit": reference.cache_hit,
        "reference_build_seconds": reference.build_seconds,
        "numeric_gate": dataclasses.asdict(gate),
    }
    if reference.executable is None:
        status = (ResultStatus.GENERATOR_GAP
                  if reference.error and reference.error.startswith("stanc_fail")
                  else ResultStatus.HARNESS_ERROR)
        reason = ("stanc rejected a cataloged construct source."
                  if status == ResultStatus.GENERATOR_GAP
                  else "Reference construct compilation failed.")
        return _result(
            case, status, reason,
            {**base_details, "reference_error": reference.error}, repro,
            started)

    worker = pathlib.Path(__file__).with_name("model_worker.py")
    environment = worker_environment(pythonpath)
    reference_command = [str(python_executable), "-u", str(worker),
                         str(reference.executable), "--backend", "reference",
                         "--data", str(case.data)]
    stanli_command = [str(python_executable), "-u", str(worker),
                      str(reference.source), "--backend", "stanli",
                      "--data", str(case.data)]
    try:
        with JsonLinesClient(
                reference_command, timeout=timeout,
                env=environment, label="reference") as ref, JsonLinesClient(
                    stanli_command, timeout=timeout,
                    env=environment, label="stanli") as ours:
            reference_description = ref.request({"action": "describe"})
            _observation(reference_description)
            try:
                stanli_description = ours.request({"action": "describe"})
            except ProtocolError as exc:
                transport = _stanli_transport_outcome(
                    case, reference_description, "construction", exc)
                return _result(
                    case, transport.status, transport.reason,
                    {**base_details, **dict(transport.details)}, repro,
                    started)
            construction = _construction_outcome(
                case, reference_description, stanli_description)
            if construction is not None:
                return _result(
                    case, construction.status, construction.reason,
                    {**base_details, **dict(construction.details)}, repro,
                    started)

            width = int(reference_description["parameter_count"])
            points = case.points or _profile_points(case.point_profile, width)
            if not points:
                return _result(
                    case, ResultStatus.GENERATOR_GAP,
                    "An accepted construct has no deterministic points.",
                    base_details, repro, started)
            if any(len(point) != width for point in points):
                return _result(
                    case, ResultStatus.GENERATOR_GAP,
                    "Catalog point width does not match the reference "
                    f"parameter count {width}.",
                    {**base_details, "points": [list(x) for x in points]},
                    repro, started)

            include_outputs = case.oracle_gate == "write_array"
            outcomes = []
            for index, point in enumerate(points):
                request = {"point": list(point),
                           "include_outputs": include_outputs}
                reference_response = ref.request(request)
                _observation(reference_response)
                try:
                    stanli_response = ours.request(request)
                except ProtocolError as exc:
                    transport = _stanli_transport_outcome(
                        case, reference_response,
                        "write_array" if include_outputs else "evaluation",
                        exc)
                    return _result(
                        case, transport.status, transport.reason,
                        {**base_details,
                         "point_index": index,
                         "point": list(point),
                         **dict(transport.details)}, repro, started)
                comparison = compare_observations(
                    _observation(stanli_response),
                    _observation(reference_response),
                    Expectation(
                        should_accept=case.expected_accept,
                        phase=case.expected_phase,
                        exception_category=case.expected_exception),
                    gate)
                outcomes.append(dataclasses.replace(
                    comparison,
                    details={"point_index": index, "point": list(point),
                             **dict(comparison.details)}))

            chosen = min(outcomes, key=lambda item: _PRECEDENCE[item.status])
            status = chosen.status
            reason = chosen.reason
            if status == ResultStatus.VERIFIED \
                    and case.oracle_gate == "numeric" and width:
                gradients = []
                for outcome in outcomes:
                    reference_raw = outcome.details.get("reference")
                    if isinstance(reference_raw, dict):
                        gradients.append(
                            Observation.from_dict(reference_raw).gradient)
                missing = [lane for lane in range(width)
                           if not any(lane < len(gradient)
                                      and math.isfinite(gradient[lane])
                                      and gradient[lane] != 0.0
                                      for gradient in gradients)]
                if missing:
                    status = ResultStatus.GENERATOR_GAP
                    reason = ("Reference gradients are uninformative for "
                              f"construct lanes {missing}.")
            return _result(
                case, status, reason,
                {**base_details,
                 "parameter_count": width,
                 "parameter_names": reference_description.get(
                     "parameter_names", []),
                 "points": [dict(outcome.details) for outcome in outcomes],
                 "selected_outcome": dict(chosen.details)},
                repro, started)
    except (OSError, ProtocolError, RuntimeError, ValueError, KeyError) as exc:
        return _result(
            case, ResultStatus.HARNESS_ERROR,
            f"Could not run paired construct processes: {exc}",
            base_details, repro, started)


def run_construct_phase(
        report: ConformanceReport, catalog: ConstructCatalog,
        policy: CapabilityPolicy, cmdstan: pathlib.Path, stanc: pathlib.Path,
        deps: pathlib.Path,
        cache_root: pathlib.Path, repo: pathlib.Path,
        python_executable: pathlib.Path = pathlib.Path(sys.executable),
        pythonpath: Optional[pathlib.Path] = None,
        jobs: int = 1, timeout: float = 60.0) -> ConformanceReport:
    """Replace selected construct gaps with paired semantic observations."""
    selected = {result.case_id: result for result in report.results
                if result.kind == "construct"}
    cases = tuple(
        case for case in catalog.cases
        if (case.case_id in selected and case.oracle_gate != "census"
            and selected[case.case_id].status == ResultStatus.GENERATOR_GAP))
    if not cases:
        return report
    gates = {case.case_id: _numeric_gate(case, policy) for case in cases}
    try:
        versions = validate_toolchain(cmdstan, stanc, deps)
        identity = transport_identity(
            python_executable, pythonpath, versions.bridgestan_version)
        prepare_reference_runtime(cmdstan, stanc, deps, versions)
    except (OracleError, TransportError) as exc:
        replacements = {
            case.case_id: _result(
                case, ResultStatus.HARNESS_ERROR,
                f"Conformance runtime validation failed: {exc}",
                {"phase": "runtime_validation"},
                selected[case.case_id].repro_command, time.monotonic())
            for case in cases
        }
        return _replace(
            report, replacements,
            {"construct_generator_version": CONSTRUCT_GENERATOR_VERSION,
             "construct_runtime_error": str(exc)})

    cache_root.mkdir(parents=True, exist_ok=True)
    # One binary per unique source; data variants reuse it in separate
    # long-lived processes so construction phase remains case-specific.
    representatives: Dict[str, ConstructCase] = {}
    for case in cases:
        representatives.setdefault(case.source_sha256, case)
    source_cases = tuple(representatives.values())
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        source_builds = tuple(pool.map(
            lambda case: _build_one(
                case, cmdstan, stanc, cache_root, deps, versions),
            source_cases))
    builds = {case.source_sha256: build
              for case, build in zip(source_cases, source_builds)}

    def evaluate_one(case: ConstructCase) -> CaseResult:
        return _evaluate_case(
            case, builds[case.source_sha256], python_executable, pythonpath,
            timeout, repo, selected[case.case_id].repro_command,
            gates[case.case_id])

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        evaluated = tuple(pool.map(evaluate_one, cases))
    replacements = {result.case_id: result for result in evaluated}
    return _replace(
        report, replacements,
        {"construct_generator_version": CONSTRUCT_GENERATOR_VERSION,
         "construct_cases": len(cases),
         "construct_sources": len(source_cases),
         "construct_reference_cache_hits": sum(
             build.cache_hit for build in source_builds),
         "construct_reference_build_seconds": sum(
             build.build_seconds for build in source_builds),
         "construct_reference_backend": "BridgeStan",
         "construct_numeric_policies": sorted(
             {case.numeric_policy for case in cases
              if case.numeric_policy is not None}),
         "construct_stanli_transport_identity": identity})


def _replace(report: ConformanceReport,
             replacements: Mapping[str, CaseResult],
             tool_details: Mapping[str, object]) -> ConformanceReport:
    results = tuple(sorted(
        (replacements.get(result.case_id, result) for result in report.results),
        key=lambda result: result.case_id))
    return dataclasses.replace(
        report, results=results,
        tools={**dict(report.tools), **dict(tool_details)})
