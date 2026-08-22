"""Evaluate generated cases through paired JSON-lines shard processes."""

from __future__ import annotations

import dataclasses
import math
import time
from typing import Dict, Mapping, Optional, Protocol, Tuple

from .compare import (Expectation, Gate, NumericComparison, Observation,
                      OutcomeComparison, compare_observations)
from .generate import GeneratedCase
from .policy import ClassificationRule
from .status import CaseResult, ResultStatus


class RequestClient(Protocol):
    def request(self, payload: Mapping[str, object]) -> Dict[str, object]:
        ...


@dataclasses.dataclass(frozen=True)
class _PointResult:
    comparison: OutcomeComparison
    reference: Optional[Observation] = None
    stanli: Optional[Observation] = None


_PRECEDENCE = {
    ResultStatus.HARNESS_ERROR: 0,
    ResultStatus.CRASHED: 1,
    ResultStatus.GENERATOR_GAP: 2,
    ResultStatus.MISMATCH: 3,
    ResultStatus.UNEXPECTED_UNSUPPORTED: 4,
    ResultStatus.EXPECTED_UNSUPPORTED: 5,
    ResultStatus.INAPPLICABLE: 6,
    ResultStatus.VERIFIED: 7,
}


def _inactive_nonzero(observation: Observation, case: GeneratedCase) \
        -> Tuple[int, ...]:
    active = range(case.parameter_offset,
                   case.parameter_offset + case.parameter_count)
    active_set = frozenset(active)
    return tuple(index for index, value in enumerate(observation.gradient)
                 if index not in active_set and value != 0.0)


def _active_point(case: GeneratedCase, point_index: int) \
        -> Dict[str, object]:
    return {
        "point_index": point_index,
        "point": list(case.active_slice(case.points[point_index])),
        "point_offset": case.parameter_offset,
        "point_total": len(case.points[point_index]),
        "point_encoding": "active_slice",
    }


def _compact_observation(observation: Observation,
                         case: GeneratedCase) -> Dict[str, object]:
    result = observation.to_dict()
    result["gradient"] = list(case.active_slice(observation.gradient))
    result["gradient_offset"] = case.parameter_offset
    result["gradient_total"] = len(observation.gradient)
    result["gradient_encoding"] = "active_slice"
    return result


def _point_outcome(case: GeneratedCase, point_index: int,
                   reference_client: RequestClient,
                   stanli_client: RequestClient, gate: Gate) -> _PointResult:
    payload = {"active_case": case.active_case,
               "point": list(case.points[point_index])}
    details = _active_point(case, point_index)
    try:
        reference_response = reference_client.request(payload)
        if reference_response.get("protocol_error"):
            raise RuntimeError(reference_response.get("message",
                                                      "reference protocol error"))
        reference = Observation.from_dict(reference_response)
    except Exception as exc:
        return _PointResult(OutcomeComparison(
            ResultStatus.HARNESS_ERROR,
            f"point {point_index}: reference protocol failure: {exc}",
            details))

    try:
        stanli_response = stanli_client.request(payload)
    except Exception as exc:
        # The worker stopped answering rather than declining the case: a
        # refusal comes back as a response with accepted=False. Blocking
        # either way, whatever the reference did with the point -- a
        # process that died reported no capability (status.py).
        details["reference"] = _compact_observation(reference, case)
        return _PointResult(OutcomeComparison(
            ResultStatus.CRASHED,
            f"point {point_index}: the stanli worker stopped answering: "
            + str(exc), details), reference)

    try:
        if stanli_response.get("protocol_error"):
            raise RuntimeError(stanli_response.get("message",
                                                   "stanli protocol error"))
        stanli = Observation.from_dict(stanli_response)
    except Exception as exc:
        details["reference"] = _compact_observation(reference, case)
        return _PointResult(OutcomeComparison(
            ResultStatus.HARNESS_ERROR,
            f"point {point_index}: stanli protocol failure: {exc}",
            details), reference)

    if reference.accepted:
        inactive_reference = _inactive_nonzero(reference, case)
        if inactive_reference:
            details.update({"reference": _compact_observation(reference, case),
                            "inactive_lanes": list(inactive_reference)})
            return _PointResult(OutcomeComparison(
                ResultStatus.GENERATOR_GAP,
                "The generated reference case depends on inactive shard lanes.",
                details), reference, stanli)
    if stanli.accepted:
        inactive_stanli = _inactive_nonzero(stanli, case)
        if inactive_stanli:
            details.update({"stanli": _compact_observation(stanli, case),
                            "inactive_lanes": list(inactive_stanli)})
            return _PointResult(OutcomeComparison(
                ResultStatus.MISMATCH,
                "stanli's active case depends on inactive shard lanes.",
                details), reference, stanli)
    outcome = compare_observations(stanli, reference, Expectation(), gate)
    outcome_details = dict(outcome.details)
    if "reference" in outcome_details:
        outcome_details["reference"] = _compact_observation(reference, case)
    if "stanli" in outcome_details:
        outcome_details["stanli"] = _compact_observation(stanli, case)
    comparison = dataclasses.replace(
        outcome, details={**details, **outcome_details})
    return _PointResult(comparison, reference, stanli)


def evaluate_generated_case(case: GeneratedCase,
                            reference_client: RequestClient,
                            stanli_client: RequestClient, gate: Gate,
                            policy_rule: Optional[ClassificationRule] = None,
                            repro_command: Optional[str] = None) -> CaseResult:
    started = time.monotonic()
    point_results = [
        _point_outcome(case, index, reference_client, stanli_client, gate)
        for index in range(len(case.points))
    ]
    outcomes = [item.comparison for item in point_results]
    chosen = min(outcomes, key=lambda item: _PRECEDENCE[item.status])

    # A deliberate capability exception still gets probed.  Refusal after the
    # reference succeeds is expected; successful parity is an improvement and
    # carries the rule so the report makes the policy stale.
    status = chosen.status
    if (status == ResultStatus.UNEXPECTED_UNSUPPORTED
            and policy_rule is not None):
        status = ResultStatus.EXPECTED_UNSUPPORTED

    if status == ResultStatus.VERIFIED:
        # Every differentiable input lane must be informative at at least one
        # reference point.  A zero gradient throughout is not proof that the
        # signature is inapplicable; it is a deterministic generator gap.
        reference_gradients = []
        for point_result in point_results:
            if point_result.reference is None:
                status = ResultStatus.HARNESS_ERROR
                chosen = OutcomeComparison(
                    status, "numeric comparison omitted its reference result", {})
                break
            reference_gradients.append(point_result.reference.gradient)
        if status == ResultStatus.VERIFIED:
            missing = []
            for lane in range(case.parameter_count):
                if lane in case.spec.expected_zero_lanes:
                    continue
                absolute = case.parameter_offset + lane
                if not any(absolute < len(gradient)
                           and math.isfinite(gradient[absolute])
                           and gradient[absolute] != 0.0
                           for gradient in reference_gradients):
                    missing.append(absolute)
            if missing:
                status = ResultStatus.GENERATOR_GAP
                chosen = OutcomeComparison(
                    status,
                    "Reference gradients are uninformative for generated "
                    f"lanes {missing}.", {"uninformative_lanes": missing})

    reason = chosen.reason
    if status == ResultStatus.EXPECTED_UNSUPPORTED and policy_rule is not None:
        reason = policy_rule.reason
    return CaseResult(
        case_id=case.case_id,
        inventory_id=case.inventory_id,
        family=case.spec.signature.name,
        kind="signature",
        status=status,
        reason=reason,
        policy_rule=policy_rule.id if policy_rule else None,
        policy_reason=policy_rule.reason if policy_rule else None,
        repro_command=repro_command,
        probe_attempted=True,
        details={
            "domain_profile": case.spec.domain_profile,
            "active_case": case.active_case,
            "parameter_offset": case.parameter_offset,
            "parameter_count": case.parameter_count,
            "weight": case.weight,
            "points": [dict(outcome.details) for outcome in outcomes],
            "selected_outcome": {
                "point_index": chosen.details.get("point_index"),
                "status": chosen.status.value,
                "reason": chosen.reason,
            },
        },
        timings={"evaluation_seconds": time.monotonic() - started},
    )
