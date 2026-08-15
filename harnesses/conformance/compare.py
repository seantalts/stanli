"""Shape-aware numeric and semantic comparison gates."""

from __future__ import annotations

import dataclasses
import math
import struct
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Union

from .status import ResultStatus


Number = Union[int, float]
NumericValue = Union[Number, Sequence["NumericValue"]]


def _numeric_from_json(value: object) -> NumericValue:
    if isinstance(value, list):
        return [_numeric_from_json(item) for item in value]
    if isinstance(value, (str, int, float)) and not isinstance(value, bool):
        return float(value)
    raise TypeError(f"numeric output contains {type(value).__name__}")


@dataclasses.dataclass(frozen=True)
class Gate:
    max_ulp: Optional[int] = 0
    abs_tol: float = 0.0
    rel_tol: float = 0.0

    def __post_init__(self) -> None:
        if self.max_ulp is not None and self.max_ulp < 0:
            raise ValueError("max_ulp must be nonnegative or None")
        if self.abs_tol < 0 or self.rel_tol < 0:
            raise ValueError("numeric tolerances must be nonnegative")


# The gate a case gets when no policy rule names one. Reviewed decision
# (2026-08-15, raised from 5 the same day): a lane within 10 ULP of the
# reference is green, so the mismatch bucket holds disagreements someone
# should read, not accumulated last-bit noise from equivalent-but-
# differently-grouped arithmetic. The raise absorbed the 6-9 ULP class the
# first live runs surfaced -- deep probe expression trees over functions
# whose forward is literally the same stan-math call (inv_Phi at 6-7 ULP
# was the type case). Anything looser than this stays a reviewed
# [[numeric_gate]] rule in policy.toml.
DEFAULT_GATE = Gate(max_ulp=10)


@dataclasses.dataclass(frozen=True)
class NumericComparison:
    agrees: bool
    compared: int
    worst_abs: float
    worst_relative: float
    worst_ulp: Optional[int]
    worst_path: Optional[str]
    reason: str

    def to_dict(self) -> Dict[str, object]:
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class Observation:
    accepted: bool
    value: Optional[float] = None
    gradient: Tuple[float, ...] = ()
    outputs: Optional[NumericValue] = None
    output_names: Tuple[str, ...] = ()
    phase: Optional[str] = None
    exception_category: Optional[str] = None
    message: Optional[str] = None

    @classmethod
    def from_dict(cls, value: Mapping[str, object]) -> "Observation":
        return cls(
            accepted=bool(value["accepted"]),
            value=(float(value["value"])
                   if value.get("value") is not None else None),
            gradient=tuple(float(x) for x in value.get("gradient", [])),
            outputs=(_numeric_from_json(value["outputs"])
                     if value.get("outputs") is not None else None),
            output_names=tuple(str(x) for x in value.get("output_names", [])),
            phase=(str(value["phase"])
                   if value.get("phase") is not None else None),
            exception_category=(str(value["exception_category"])
                                if value.get("exception_category") is not None
                                else None),
            message=(str(value["message"])
                     if value.get("message") is not None else None),
        )

    def to_dict(self) -> Dict[str, object]:
        return {
            "accepted": self.accepted,
            "value": self.value,
            "gradient": list(self.gradient),
            "outputs": self.outputs,
            "output_names": list(self.output_names),
            "phase": self.phase,
            "exception_category": self.exception_category,
            "message": self.message,
        }


@dataclasses.dataclass(frozen=True)
class Expectation:
    should_accept: bool = True
    phase: Optional[str] = None
    exception_category: Optional[str] = None
    nan_paths: Tuple[str, ...] = ()
    infinity_paths: Tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True)
class OutcomeComparison:
    status: ResultStatus
    reason: str
    details: Mapping[str, object]


def _shape(value: NumericValue) -> Tuple[int, ...]:
    if isinstance(value, (int, float)):
        return ()
    if isinstance(value, (str, bytes)):
        raise TypeError("numeric output cannot be text")
    values = list(value)
    if not values:
        return (0,)
    child = _shape(values[0])
    for item in values[1:]:
        if _shape(item) != child:
            raise ValueError("numeric output is ragged")
    return (len(values),) + child


def _flatten(value: NumericValue, path: str) -> List[Tuple[str, float]]:
    if isinstance(value, (int, float)):
        return [(path, float(value))]
    if isinstance(value, (str, bytes)):
        raise TypeError("numeric output cannot be text")
    flattened: List[Tuple[str, float]] = []
    for index, item in enumerate(value):
        flattened.extend(_flatten(item, f"{path}[{index}]"))
    return flattened


def ulp_distance(left: float, right: float) -> Optional[int]:
    """Distance in the monotone IEEE-754 ordering; None for nonfinite."""
    if not math.isfinite(left) or not math.isfinite(right):
        return None
    # IEEE equality intentionally makes the two signed zeros agree.  This
    # matches Stan's numeric semantics and the pre-existing sweep contract.
    if left == right:
        return 0
    sign = 1 << 63
    mask = (1 << 64) - 1

    def key(value: float) -> int:
        bits = struct.unpack(">Q", struct.pack(">d", value))[0]
        return ((~bits) & mask) if bits & sign else (bits | sign)

    return abs(key(left) - key(right))


def _finite_agrees(got: float, reference: float, gate: Gate) \
        -> Tuple[bool, float, float, int]:
    difference = abs(got - reference)
    if difference == 0.0:
        return True, 0.0, 0.0, 0
    scale = max(abs(got), abs(reference))
    relative = difference / scale if scale else math.inf
    ulp = ulp_distance(got, reference)
    assert ulp is not None
    tolerance_pass = difference <= max(gate.abs_tol, gate.rel_tol * scale)
    ulp_pass = gate.max_ulp is not None and ulp <= gate.max_ulp
    return tolerance_pass or ulp_pass, difference, relative, ulp


def compare_numeric(got: NumericValue, reference: NumericValue, gate: Gate,
                    nan_paths: Iterable[str] = (), root: str = "value") \
        -> NumericComparison:
    try:
        got_shape, reference_shape = _shape(got), _shape(reference)
    except (TypeError, ValueError) as exc:
        return NumericComparison(False, 0, math.inf, math.inf, None, root,
                                 str(exc))
    if got_shape != reference_shape:
        return NumericComparison(
            False, 0, math.inf, math.inf, None, root,
            f"shape mismatch: stanli {got_shape}, reference {reference_shape}")

    got_values = _flatten(got, root)
    reference_values = _flatten(reference, root)
    allowed_nan = frozenset(nan_paths)
    agrees = True
    worst_abs = 0.0
    worst_relative = 0.0
    worst_ulp = 0
    worst_path: Optional[str] = None
    first_failure: Optional[str] = None
    for (path, actual), (_, expected) in zip(got_values, reference_values):
        if math.isnan(actual) or math.isnan(expected):
            lane_agrees = (math.isnan(actual) and math.isnan(expected)
                            and path in allowed_nan)
            absolute = relative = 0.0 if lane_agrees else math.inf
            ulp = None
            lane_reason = ("expected NaN" if lane_agrees
                           else "NaN outside an explicit semantic expectation")
        elif math.isinf(actual) or math.isinf(expected):
            lane_agrees = actual == expected
            absolute = relative = 0.0 if lane_agrees else math.inf
            ulp = None
            lane_reason = ("same signed infinity" if lane_agrees
                           else "finite/nonfinite or signed-infinity mismatch")
        else:
            lane_agrees, absolute, relative, finite_ulp = _finite_agrees(
                actual, expected, gate)
            ulp = finite_ulp
            lane_reason = (f"{absolute:.17g} absolute, {relative:.3g} relative, "
                           f"{ulp} ULP")
        if absolute > worst_abs or (absolute == worst_abs
                                    and relative > worst_relative):
            worst_abs, worst_relative = absolute, relative
            worst_path = path
        if ulp is not None:
            worst_ulp = max(worst_ulp, ulp)
        if not lane_agrees:
            agrees = False
            if first_failure is None:
                first_failure = f"{path}: {lane_reason}"
    reason = ("all numeric lanes satisfy the gate" if agrees
              else first_failure or "numeric mismatch")
    return NumericComparison(agrees, len(got_values), worst_abs,
                             worst_relative, worst_ulp, worst_path, reason)


def compare_observations(got: Observation, reference: Observation,
                         expectation: Expectation, gate: Gate) \
        -> OutcomeComparison:
    common = {"stanli": got.to_dict(), "reference": reference.to_dict()}
    if expectation.should_accept:
        if not reference.accepted:
            # The generated nominal point is not valid according to the
            # oracle, regardless of what stanli happened to do with it.
            return OutcomeComparison(
                ResultStatus.GENERATOR_GAP,
                "The reference rejected a nominally valid generated case: "
                + (reference.message or reference.exception_category
                   or "unknown rejection"),
                common)
        if not got.accepted:
            return OutcomeComparison(
                ResultStatus.UNEXPECTED_UNSUPPORTED,
                "The reference accepted the case but stanli rejected it: "
                + (got.message or got.exception_category or "unknown rejection"),
                common)
        if got.value is None or reference.value is None:
            return OutcomeComparison(
                ResultStatus.HARNESS_ERROR,
                "An accepted observation omitted its log density.", common)
        unexpected_nonfinite = _unexpected_reference_nonfinite(reference,
                                                                expectation)
        if unexpected_nonfinite:
            return OutcomeComparison(
                ResultStatus.GENERATOR_GAP,
                "The nominal finite reference point produced unexpected "
                "nonfinite lanes: " + ", ".join(unexpected_nonfinite), common)
        comparisons = [
            compare_numeric(got.value, reference.value, gate,
                            expectation.nan_paths, root="value"),
            compare_numeric(list(got.gradient), list(reference.gradient), gate,
                            expectation.nan_paths, root="gradient"),
        ]
        # Optional deterministic outputs are compared after lp and gradient.
        if got.outputs is not None or reference.outputs is not None:
            if got.outputs is None or reference.outputs is None:
                return OutcomeComparison(
                    ResultStatus.MISMATCH,
                    "Only one implementation returned deterministic outputs.",
                    common)
            if got.output_names != reference.output_names:
                return OutcomeComparison(
                    ResultStatus.MISMATCH,
                    "Constrained output names or order disagree.", common)
            comparisons.append(compare_numeric(
                got.outputs, reference.outputs, gate, expectation.nan_paths,
                root="outputs"))
        comparison = _combine(comparisons)
        details = {**common, "comparison": comparison.to_dict()}
        return OutcomeComparison(
            ResultStatus.VERIFIED if comparison.agrees else ResultStatus.MISMATCH,
            comparison.reason, details)

    # Explicit semantic rejection case: the reference defines the phase and
    # exception category; checked-in expectations can narrow them further.
    if reference.accepted:
        return OutcomeComparison(
            ResultStatus.GENERATOR_GAP,
            "The reference accepted a case generated to reject.", common)
    if got.accepted:
        return OutcomeComparison(
            ResultStatus.MISMATCH,
            "stanli accepted a case the reference rejected.", common)
    reference_differences = []
    if expectation.phase is not None and reference.phase != expectation.phase:
        reference_differences.append(
            f"reference phase {reference.phase!r} != catalog "
            f"{expectation.phase!r}")
    if (expectation.exception_category is not None
            and reference.exception_category != expectation.exception_category):
        reference_differences.append(
            f"reference exception {reference.exception_category!r} != catalog "
            f"{expectation.exception_category!r}")
    if reference_differences:
        return OutcomeComparison(ResultStatus.GENERATOR_GAP,
                                 "; ".join(reference_differences), common)
    wanted_phase = expectation.phase or reference.phase
    wanted_category = expectation.exception_category or reference.exception_category
    differences = []
    if got.phase != wanted_phase:
        differences.append(f"phase {got.phase!r} != {wanted_phase!r}")
    if got.exception_category != wanted_category:
        differences.append(
            f"exception {got.exception_category!r} != {wanted_category!r}")
    if differences:
        return OutcomeComparison(ResultStatus.MISMATCH, "; ".join(differences),
                                 common)
    return OutcomeComparison(ResultStatus.VERIFIED,
                             "Both implementations rejected in the expected phase.",
                             common)


def _combine(comparisons: Sequence[NumericComparison]) -> NumericComparison:
    if not comparisons:
        return NumericComparison(True, 0, 0.0, 0.0, 0, None,
                                 "no numeric lanes")
    agrees = all(item.agrees for item in comparisons)
    worst_abs_item = max(comparisons,
                         key=lambda item: (item.worst_abs,
                                           item.worst_relative))
    finite_ulps = [item.worst_ulp for item in comparisons
                   if item.worst_ulp is not None]
    first_failure = next((item.reason for item in comparisons
                          if not item.agrees), None)
    return NumericComparison(
        agrees=agrees,
        compared=sum(item.compared for item in comparisons),
        worst_abs=worst_abs_item.worst_abs,
        worst_relative=max(item.worst_relative for item in comparisons),
        worst_ulp=max(finite_ulps) if finite_ulps else None,
        worst_path=worst_abs_item.worst_path,
        reason=("all numeric lanes satisfy the gate" if agrees
                else first_failure or "numeric mismatch"),
    )


def _unexpected_reference_nonfinite(
        observation: Observation, expectation: Expectation) -> Tuple[str, ...]:
    lanes: List[Tuple[str, float]] = []
    if observation.value is not None:
        lanes.append(("value", observation.value))
    lanes.extend((f"gradient[{index}]", value)
                 for index, value in enumerate(observation.gradient))
    if observation.outputs is not None:
        try:
            lanes.extend(_flatten(observation.outputs, "outputs"))
        except (TypeError, ValueError):
            # Shape/raggedness remains a comparison mismatch; this helper is
            # concerned only with semantic nonfinite expectations.
            pass
    allowed_nan = frozenset(expectation.nan_paths)
    allowed_infinity = frozenset(expectation.infinity_paths)
    unexpected = []
    for path, value in lanes:
        if math.isnan(value) and path not in allowed_nan:
            unexpected.append(path + "=NaN")
        elif math.isinf(value) and path not in allowed_infinity:
            unexpected.append(path + ("=+inf" if value > 0 else "=-inf"))
    return tuple(unexpected)
