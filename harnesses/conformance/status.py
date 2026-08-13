"""Result vocabulary shared by generators, evaluators, and reports."""

from __future__ import annotations

import dataclasses
import enum
from typing import Dict, Mapping, Optional


class ResultStatus(str, enum.Enum):
    VERIFIED = "verified"
    EXPECTED_UNSUPPORTED = "expected_unsupported"
    INAPPLICABLE = "inapplicable"
    UNEXPECTED_UNSUPPORTED = "unexpected_unsupported"
    MISMATCH = "mismatch"
    GENERATOR_GAP = "generator_gap"
    HARNESS_ERROR = "harness_error"

    @property
    def is_blocking(self) -> bool:
        return self in BLOCKING_STATUSES


BLOCKING_STATUSES = frozenset(
    {
        ResultStatus.UNEXPECTED_UNSUPPORTED,
        ResultStatus.MISMATCH,
        ResultStatus.GENERATOR_GAP,
        ResultStatus.HARNESS_ERROR,
    }
)

NON_NUMERIC_STATUSES = frozenset(
    {ResultStatus.EXPECTED_UNSUPPORTED, ResultStatus.INAPPLICABLE}
)


@dataclasses.dataclass(frozen=True)
class CaseResult:
    case_id: str
    inventory_id: str
    family: str
    kind: str
    status: ResultStatus
    reason: str
    policy_rule: Optional[str] = None
    policy_reason: Optional[str] = None
    repro_command: Optional[str] = None
    probe_attempted: bool = False
    details: Mapping[str, object] = dataclasses.field(default_factory=dict)
    timings: Mapping[str, float] = dataclasses.field(default_factory=dict)

    def __post_init__(self) -> None:
        if not self.case_id:
            raise ValueError("case_id must not be empty")
        if not self.inventory_id:
            raise ValueError("inventory_id must not be empty")
        if not self.reason:
            raise ValueError(f"{self.case_id}: every result needs a reason")
        if self.status == ResultStatus.EXPECTED_UNSUPPORTED:
            if not self.policy_rule or not self.policy_reason:
                raise ValueError(
                    f"{self.case_id}: expected_unsupported requires a policy")

    def to_dict(self) -> Dict[str, object]:
        result: Dict[str, object] = {
            "case_id": self.case_id,
            "inventory_id": self.inventory_id,
            "family": self.family,
            "kind": self.kind,
            "status": self.status.value,
            "reason": self.reason,
            "probe_attempted": self.probe_attempted,
            "details": dict(self.details),
            "timings": dict(self.timings),
        }
        if self.policy_rule is not None:
            result["policy_rule"] = self.policy_rule
        if self.policy_reason is not None:
            result["policy_reason"] = self.policy_reason
        if self.repro_command is not None:
            result["repro_command"] = self.repro_command
        return result

    @classmethod
    def from_dict(cls, value: Mapping[str, object]) -> "CaseResult":
        return cls(
            case_id=str(value["case_id"]),
            inventory_id=str(value["inventory_id"]),
            family=str(value["family"]),
            kind=str(value["kind"]),
            status=ResultStatus(str(value["status"])),
            reason=str(value["reason"]),
            policy_rule=(str(value["policy_rule"])
                         if value.get("policy_rule") is not None else None),
            policy_reason=(str(value["policy_reason"])
                           if value.get("policy_reason") is not None else None),
            repro_command=(str(value["repro_command"])
                           if value.get("repro_command") is not None else None),
            probe_attempted=bool(value.get("probe_attempted", False)),
            details=dict(value.get("details", {})),
            timings={str(k): float(v)
                     for k, v in dict(value.get("timings", {})).items()},
        )
