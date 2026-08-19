"""Deterministic type-directed cases and content-addressed Stan shards."""

from __future__ import annotations

import dataclasses
import hashlib
import itertools
from typing import Callable, Dict, Optional, Sequence, Tuple, Union

from .domains import (probability_values, scalar_values,
                      semantic_inapplicability)
from .signatures import ArrayType, AtomicType, Inventory, Signature, StanType


POINT_LANES = (
    (),  # Expanded to all-zero after the shard has a known parameter width.
    (0.30, -0.20, 0.10, -0.35, 0.25, -0.15, 0.40, -0.05),
    (-0.40, 0.15, -0.25, 0.05, -0.10, 0.35, -0.30, 0.20),
)

GENERATED_CONTAINERS = frozenset({"vector", "row_vector", "matrix"})
ELEMENT_OFFSETS = (-0.05, 0.04, -0.02, 0.06)
GENERATED_ATOMS = frozenset({"int", "real", *GENERATED_CONTAINERS})


@dataclasses.dataclass(frozen=True)
class ScalarCaseSpec:
    signature: Signature
    centers: Tuple[object, ...]
    domain_profile: str
    perturbation: float
    data_positions: Tuple[int, ...] = ()
    expected_zero_lanes: Tuple[int, ...] = ()

    @property
    def case_id(self) -> str:
        return self.signature.case_id

    @property
    def inventory_id(self) -> str:
        return self.signature.canonical_id

    @property
    def parameter_count(self) -> int:
        return sum(isinstance(argument, AtomicType)
                   and argument.name == "real"
                   and index not in self.data_positions
                   for index, argument in enumerate(self.signature.arguments))

    def render_call(self, offset: int) -> str:
        arguments = []
        lane = 0
        for index, (argument, center) in enumerate(
                zip(self.signature.arguments, self.centers)):
            if (isinstance(argument, AtomicType) and argument.name == "real"
                    and index not in self.data_positions):
                arguments.append(
                    f"({float(center):.17g} + {self.perturbation:.17g} "
                    f"* theta[{offset + lane + 1}])")
                lane += 1
            elif isinstance(argument, AtomicType) \
                    and argument.name in ("int", "real"):
                arguments.append(str(int(center)) if argument.name == "int"
                                 else f"{float(center):.17g}")
            else:  # guarded by scalar_case_for, kept loud for direct callers
                raise ValueError(f"{self.inventory_id}: non-scalar argument")
        if self.signature.is_probability and len(arguments) > 1:
            rendered = arguments[0] + " | " + ", ".join(arguments[1:])
        else:
            rendered = ", ".join(arguments)
        return f"{self.signature.name}({rendered})"

    def render_body(self, offset: int, weight: float) -> Tuple[str, ...]:
        return (f"contribution += {weight:.17g} * {self.render_call(offset)};",)


def _collapsed_atom(value: StanType) -> Optional[str]:
    """Collapse an array/container type to its scalar overload atom."""
    while isinstance(value, ArrayType):
        value = value.element
    if not isinstance(value, AtomicType) or value.name not in GENERATED_ATOMS:
        return None
    return "real" if value.name in {"vector", "row_vector", "matrix"} \
        else value.name


def _vectorization_depth(value: StanType) -> int:
    if isinstance(value, ArrayType):
        return value.rank + _vectorization_depth(value.element)
    if not isinstance(value, AtomicType):
        return 0
    if value.name in ("vector", "row_vector"):
        return 1
    if value.name == "matrix":
        return 2
    return 0


def _shape_dimensions(signature: Signature) -> Tuple[int, ...]:
    depths = [_vectorization_depth(value)
              for value in signature.arguments + (signature.result,)]
    depth = max(depths)
    if depth == 0:
        return ()
    if depth <= 2:
        # Keep ordinary vectors length two and matrices 2x2. Square shapes
        # also cover Stan's mixed-depth matrix multiplication overloads.
        return (2,) * depth
    # Exactly one axis has a later lane, so even rank-eight overloads remain
    # tiny.  Hashing the chosen axis distributes that lane across every depth
    # in the exhaustive inventory rather than always exercising the outside.
    digest = hashlib.sha256(signature.canonical_id.encode("utf-8")).digest()
    selected = int.from_bytes(digest[:4], "big") % depth
    return tuple(2 if index == selected else 1 for index in range(depth))


def _real_lane_count(value: StanType, dimensions: Sequence[int],
                     axis: int = 0) -> int:
    if isinstance(value, ArrayType):
        array_dimensions = dimensions[axis:axis + value.rank]
        return (_product(array_dimensions)
                * _real_lane_count(value.element, dimensions,
                                   axis + value.rank))
    if not isinstance(value, AtomicType):
        return 0
    if value.name == "real":
        return 1
    if value.name in ("vector", "row_vector"):
        return dimensions[axis]
    if value.name == "matrix":
        return dimensions[axis] * dimensions[axis + 1]
    return 0


def _product(values: Sequence[int]) -> int:
    result = 1
    for value in values:
        result *= value
    return result


def _type_declaration(value: StanType, name: str,
                      dimensions: Sequence[int], axis: int = 0) -> str:
    if isinstance(value, ArrayType):
        array_dimensions = ", ".join(
            str(x) for x in dimensions[axis:axis + value.rank])
        return (f"array[{array_dimensions}] "
                f"{_type_declaration(value.element, name, dimensions, axis + value.rank)}")
    if not isinstance(value, AtomicType):
        raise ValueError(f"cannot declare generated {value.canonical()}")
    if value.name in ("int", "real"):
        return f"{value.name} {name}"
    if value.name in ("vector", "row_vector"):
        return f"{value.name}[{dimensions[axis]}] {name}"
    if value.name == "matrix":
        return f"matrix[{dimensions[axis]}, {dimensions[axis + 1]}] {name}"
    raise ValueError(f"cannot declare generated {value.canonical()}")


def _numeric_literal(value: StanType, integer: int,
                     next_real: Callable[[], str],
                     dimensions: Sequence[int], axis: int = 0) -> str:
    if isinstance(value, ArrayType):
        array_dimensions = dimensions[axis:axis + value.rank]

        def array_at(dimension: int) -> str:
            if dimension == len(array_dimensions):
                return _numeric_literal(value.element, integer, next_real,
                                        dimensions, axis + value.rank)
            members = (array_at(dimension + 1)
                       for _ in range(array_dimensions[dimension]))
            return "{" + ", ".join(members) + "}"

        return array_at(0)
    if not isinstance(value, AtomicType):
        raise ValueError(f"cannot initialize generated {value.canonical()}")
    if value.name == "int":
        return str(integer)
    if value.name == "real":
        return next_real()
    if value.name == "vector":
        return "[" + ", ".join(
            next_real() for _ in range(dimensions[axis])) + "]'"
    if value.name == "row_vector":
        return "[" + ", ".join(
            next_real() for _ in range(dimensions[axis])) + "]"
    if value.name == "matrix":
        rows = ("[" + ", ".join(
            next_real() for _ in range(dimensions[axis + 1])) + "]"
                for _ in range(dimensions[axis]))
        return "[" + ", ".join(rows) + "]"
    raise ValueError(f"cannot initialize generated {value.canonical()}")


def _result_lanes(value: StanType, expression: str,
                  dimensions: Sequence[int], axis: int = 0) \
        -> Tuple[str, ...]:
    if isinstance(value, ArrayType):
        lanes = []
        array_dimensions = dimensions[axis:axis + value.rank]
        ranges = (range(1, dimension + 1)
                  for dimension in array_dimensions)
        for indices in itertools.product(*ranges):
            suffix = ", ".join(str(index) for index in indices)
            lanes.extend(_result_lanes(value.element,
                                       f"{expression}[{suffix}]", dimensions,
                                       axis + value.rank))
        return tuple(lanes)
    if not isinstance(value, AtomicType):
        raise ValueError(f"cannot reduce generated {value.canonical()}")
    if value.name == "real":
        return (expression,)
    if value.name in ("vector", "row_vector"):
        return tuple(f"{expression}[{index}]"
                     for index in range(1, dimensions[axis] + 1))
    if value.name == "matrix":
        return tuple(f"{expression}[{row}, {column}]"
                     for row in range(1, dimensions[axis] + 1)
                     for column in range(1, dimensions[axis + 1] + 1))
    raise ValueError(f"cannot reduce generated {value.canonical()}")


@dataclasses.dataclass(frozen=True)
class VectorizedCaseSpec:
    """A recursively numeric overload backed by an accepted scalar case.

    Array dimensions are deterministic and bounded, and every real leaf has
    its own unconstrained parameter.  The exact collapsed scalar overload is
    the domain oracle; this does not inspect any stanli implementation table.
    """

    signature: Signature
    scalar: ScalarCaseSpec

    @property
    def case_id(self) -> str:
        return self.signature.case_id

    @property
    def inventory_id(self) -> str:
        return self.signature.canonical_id

    @property
    def centers(self) -> Tuple[object, ...]:
        return self.scalar.centers

    @property
    def domain_profile(self) -> str:
        return "vectorized:" + self.scalar.domain_profile

    @property
    def perturbation(self) -> float:
        return self.scalar.perturbation

    @property
    def data_positions(self) -> Tuple[int, ...]:
        return self.scalar.data_positions

    @property
    def dimensions(self) -> Tuple[int, ...]:
        return _shape_dimensions(self.signature)

    @property
    def parameter_count(self) -> int:
        return sum(_real_lane_count(argument, self.dimensions)
                   for index, argument in enumerate(self.signature.arguments)
                   if index not in self.data_positions)

    @property
    def expected_zero_lanes(self) -> Tuple[int, ...]:
        zero_scalar_lanes = frozenset(self.scalar.expected_zero_lanes)
        scalar_lane = 0
        parameter_lane = 0
        result = []
        for index, argument in enumerate(self.signature.arguments):
            if index in self.data_positions:
                continue
            count = _real_lane_count(argument, self.dimensions)
            if _collapsed_atom(argument) == "real":
                if scalar_lane in zero_scalar_lanes:
                    result.extend(range(parameter_lane,
                                        parameter_lane + count))
                scalar_lane += 1
            parameter_lane += count
        return tuple(result)

    def point_patterns(self) -> Optional[Tuple[Tuple[float, ...], ...]]:
        if self.signature.name not in {"fdim", "fmax", "fmin"}:
            return None
        lanes = []
        for index, argument in enumerate(self.signature.arguments):
            if index in self.data_positions \
                    or _collapsed_atom(argument) != "real":
                continue
            lanes.extend([2.0 if index % 2 == 0 else -2.0]
                         * _real_lane_count(argument, self.dimensions))
        return ((), tuple(lanes), tuple(-value for value in lanes))

    def render_body(self, offset: int, weight: float) -> Tuple[str, ...]:
        declarations = []
        arguments = []
        theta_lane = 0
        for index, (argument, center) in enumerate(
                zip(self.signature.arguments, self.centers)):
            real_lanes = _real_lane_count(argument, self.dimensions)
            value_lane = 0
            data_only = index in self.data_positions

            def next_real() -> str:
                nonlocal theta_lane, value_lane
                element_offset = (ELEMENT_OFFSETS[
                    value_lane % len(ELEMENT_OFFSETS)]
                                  if real_lanes > 1 else 0.0)
                value_lane += 1
                element_center = float(center) + element_offset
                if data_only:
                    return f"{element_center:.17g}"
                expression = (
                    f"({element_center:.17g} + {self.perturbation:.17g} "
                    f"* theta[{offset + theta_lane + 1}])")
                theta_lane += 1
                return expression

            name = f"conformance_arg_{index + 1}"
            literal = _numeric_literal(argument, int(center), next_real,
                                       self.dimensions)
            if data_only:
                # Pass the literal straight to the call. A local is never
                # data-only in Stan however it was initialized, so binding
                # this one to a name loses exactly the property the
                # signature requires -- "the Nth argument must be data-only"
                # -- and stanc rejects the whole shard, taking every
                # unrelated case packed beside it down as a generator gap.
                # The scalar renderer already inlines these; this is the
                # vectorized half catching up.
                arguments.append(literal)
            else:
                declarations.append(
                    f"{_type_declaration(argument, name, self.dimensions)} "
                    f"= {literal};")
                arguments.append(name)

        if theta_lane != self.parameter_count:
            raise ValueError(f"{self.inventory_id}: vectorized lane mismatch")
        if self.signature.is_probability and len(arguments) > 1:
            rendered = arguments[0] + " | " + ", ".join(arguments[1:])
        else:
            rendered = ", ".join(arguments)
        call = f"{self.signature.name}({rendered})"
        if (isinstance(self.signature.result, AtomicType)
                and self.signature.result.name == "real"):
            declarations.append(
                f"contribution += {weight:.17g} * {call};")
            return tuple(declarations)

        declarations.append(
            f"{_type_declaration(self.signature.result, 'conformance_result', self.dimensions)} "
            f"= {call};")
        terms = []
        for index, lane in enumerate(
                _result_lanes(self.signature.result, "conformance_result",
                              self.dimensions), 1):
            lane_weight = 1.0 + ((index * 5) % 11) / 32.0
            terms.append(f"{lane_weight:.17g} * {lane}")
        declarations.append(
            f"contribution += {weight:.17g} * ({' + '.join(terms)});")
        return tuple(declarations)


GeneratedSpec = Union[ScalarCaseSpec, VectorizedCaseSpec]


@dataclasses.dataclass(frozen=True)
class GeneratedCase:
    spec: GeneratedSpec
    active_case: int
    parameter_offset: int
    parameter_count: int
    weight: float
    points: Tuple[Tuple[float, ...], ...]

    @property
    def case_id(self) -> str:
        return self.spec.case_id

    @property
    def inventory_id(self) -> str:
        return self.spec.inventory_id

    def active_slice(self, values: Sequence[float]) -> Tuple[float, ...]:
        start = self.parameter_offset
        return tuple(values[start:start + self.parameter_count])


@dataclasses.dataclass(frozen=True)
class GeneratedShard:
    shard_index: int
    cases: Tuple[GeneratedCase, ...]
    source: str
    parameter_count: int
    source_sha256: str

    @property
    def id(self) -> str:
        return f"scalar-{self.shard_index:04d}-{self.source_sha256[:16]}"

    @property
    def case_ids(self) -> Tuple[str, ...]:
        return tuple(case.case_id for case in self.cases)

    def metadata(self) -> Dict[str, object]:
        return {
            "id": self.id,
            "source_sha256": self.source_sha256,
            "parameter_count": self.parameter_count,
            "case_ids": list(self.case_ids),
        }

    def result_metadata(self) -> Dict[str, object]:
        """Compact shard identity repeated on each result row."""
        return {
            "id": self.id,
            "source_sha256": self.source_sha256,
            "parameter_count": self.parameter_count,
            "case_count": len(self.cases),
        }


def scalar_case_for(signature: Signature) -> Optional[ScalarCaseSpec]:
    """Generate ordinary and support-profiled probability scalar cases.

    Probability functions use domain metadata rather than generic centers.
    Containers route to the later recursive generator.  This function never
    inspects stanli support tables.
    """
    if signature.structural_inapplicability() is not None:
        return None
    if semantic_inapplicability(signature) is not None:
        return None
    if signature.is_rng:
        return None
    if not isinstance(signature.result, AtomicType) \
            or signature.result.name != "real":
        return None
    if not signature.arguments:
        return None
    if any(not isinstance(argument, AtomicType)
           or argument.name not in ("real", "int")
           for argument in signature.arguments):
        return None
    if not any(argument.name == "real" for argument in signature.arguments):
        return None
    if signature.is_probability and probability_values(signature) is None:
        return None
    (centers, domain_profile, perturbation, data_positions,
     expected_zero_lanes) = scalar_values(signature)
    return ScalarCaseSpec(signature, centers, domain_profile, perturbation,
                          data_positions, expected_zero_lanes)


def scalar_inventory(inventory: Inventory) -> Tuple[ScalarCaseSpec, ...]:
    cases = [case for signature in inventory.signatures
             for case in [scalar_case_for(signature)] if case is not None]
    return tuple(sorted(cases, key=lambda case: case.inventory_id))


def vectorized_case_for(signature: Signature,
                        scalar_signatures: Dict[str, Signature]) \
        -> Optional[VectorizedCaseSpec]:
    """Generate recursive array/container overloads with a scalar analogue."""
    if (signature.structural_inapplicability() is not None
            or semantic_inapplicability(signature) is not None
            or signature.is_rng or not signature.arguments):
        return None
    collapsed_arguments = tuple(_collapsed_atom(argument)
                                for argument in signature.arguments)
    if any(argument is None for argument in collapsed_arguments):
        return None
    if _collapsed_atom(signature.result) != "real":
        return None
    if (all(isinstance(argument, AtomicType)
            and argument.name in ("int", "real")
            for argument in signature.arguments)
            and isinstance(signature.result, AtomicType)
            and signature.result.name == "real"):
        return None
    scalar_id = (f"{signature.name}({','.join(collapsed_arguments)})"
                 "=>real")
    scalar_signature = scalar_signatures.get(scalar_id)
    if scalar_signature is None:
        return None
    scalar = scalar_case_for(scalar_signature)
    if scalar is None:
        return None
    return VectorizedCaseSpec(signature, scalar)


def generated_inventory(inventory: Inventory) -> Tuple[GeneratedSpec, ...]:
    scalar_by_id = {signature.canonical_id: signature
                    for signature in inventory.signatures}
    cases = list(scalar_inventory(inventory))
    cases.extend(
        case for signature in inventory.signatures
        for case in [vectorized_case_for(signature, scalar_by_id)]
        if case is not None)
    ids = [case.inventory_id for case in cases]
    if len(ids) != len(set(ids)):
        raise ValueError("type-directed generators produced duplicate cases")
    return tuple(sorted(cases, key=lambda case: case.inventory_id))


def _point(width: int, offset: int, lanes: int,
           pattern: Sequence[float]) -> Tuple[float, ...]:
    point = [0.0] * width
    for lane in range(lanes):
        point[offset + lane] = pattern[lane % len(pattern)] if pattern else 0.0
    return tuple(point)


def _case_points(spec: GeneratedSpec, width: int, offset: int) \
        -> Tuple[Tuple[float, ...], ...]:
    patterns = (spec.point_patterns()
                if isinstance(spec, VectorizedCaseSpec) else None)
    return tuple(_point(width, offset, spec.parameter_count, pattern)
                 for pattern in (patterns or POINT_LANES))


def _source(specs: Sequence[GeneratedSpec], offsets: Sequence[int],
            weights: Sequence[float], parameter_count: int) -> str:
    branches = []
    for index, (spec, offset, weight) in enumerate(
            zip(specs, offsets, weights), 1):
        prefix = "if" if index == 1 else "else if"
        branches.extend([
            f"  {prefix} (active_case == {index}) {{",
            f"    // {spec.case_id}",
        ])
        branches.extend(f"    {line}"
                        for line in spec.render_body(offset, weight))
        branches.append("  }")
    return (
        "// Generated by harnesses/conformance/generate.py; do not edit.\n"
        "functions {\n"
        "  real conformance_generated_jacobian(int active_case, vector theta) {\n"
        "    real contribution = 0;\n"
        + "\n".join("  " + line for line in branches) + "\n"
        "    return contribution;\n"
        "  }\n"
        "}\n"
        "data {\n"
        f"  int<lower=1, upper={len(specs)}> active_case;\n"
        "}\n"
        "parameters {\n"
        f"  vector[{parameter_count}] theta;\n"
        "}\n"
        "transformed parameters {\n"
        "  real conformance_contribution =\n"
        "    conformance_generated_jacobian(active_case, theta);\n"
        "}\n"
        "model {\n"
        "  target += conformance_contribution;\n"
        "}\n"
    )


def make_subshard_source(cases: Sequence[GeneratedCase],
                         parameter_count: int) -> str:
    """Render a stanli retry shard while preserving reference lane offsets."""
    if not cases:
        raise ValueError("cannot render an empty retry shard")
    return _source(
        tuple(case.spec for case in cases),
        tuple(case.parameter_offset for case in cases),
        tuple(case.weight for case in cases), parameter_count)


def make_scalar_shards(specs: Sequence[GeneratedSpec], shard_size: int = 128) \
        -> Tuple[GeneratedShard, ...]:
    if shard_size < 1:
        raise ValueError("generated shard size must be positive")
    ordered = tuple(sorted(specs, key=lambda case: case.inventory_id))
    ids = [case.case_id for case in ordered]
    if len(ids) != len(set(ids)):
        raise ValueError("scalar generator received duplicate case IDs")
    shards = []
    for shard_index, start in enumerate(range(0, len(ordered), shard_size)):
        chunk = ordered[start:start + shard_size]
        offsets = []
        parameter_count = 0
        for spec in chunk:
            offsets.append(parameter_count)
            parameter_count += spec.parameter_count
        # Unequal, exactly representable dyadic weights avoid a repeated
        # reduction pattern while keeping last-bit comparison meaningful.
        weights = tuple(1.0 + ((index * 7) % 13) / 32.0
                        for index in range(1, len(chunk) + 1))
        source = _source(chunk, offsets, weights, parameter_count)
        generated = []
        for active_case, (spec, offset, weight) in enumerate(
                zip(chunk, offsets, weights), 1):
            points = _case_points(spec, parameter_count, offset)
            generated.append(GeneratedCase(spec, active_case, offset,
                                           spec.parameter_count, weight, points))
        digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
        shards.append(GeneratedShard(shard_index, tuple(generated), source,
                                     parameter_count, digest))
    return tuple(shards)
