#!/usr/bin/env python3
"""Generate exhaustive all-context fixtures for ordinary builtin overloads.

The name/kind contract comes from the unified runtime registry's JSON dump,
so the generator never parses C++ or maintains a second name list. It joins
those source names against
stanc's authoritative ``--dump-stan-math-signatures`` inventory, then every
compatible overload is emitted once.  Calls are partitioned by rendered size
so high-rank array signatures do not make one fixture disproportionately long.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import Sequence


ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "harnesses"))

from conformance.domains import scalar_values  # noqa: E402
from conformance.generate import (  # noqa: E402
    ELEMENT_OFFSETS,
    _numeric_literal,
    _shape_dimensions,
    _type_declaration,
)
from conformance.signatures import (  # noqa: E402
    ArrayType,
    AtomicType,
    Signature,
    StanType,
    load_inventory,
)
from function_signature_common import (  # noqa: E402
    RegistrySpec,
    all_context_model,
    balanced_partitions,
    generated_model_record,
    numeric_leaf_kind,
    registry_by_name,
    resolve_registry_spec,
    unwrap_data,
    write_generated_outputs,
)


DEFAULT_STANC = ROOT / "deps/stanc3/stanc"
DEFAULT_REGISTRY = ROOT / "build/dump_function_specs"
DEFAULT_OUTPUT_DIR = ROOT / "tests/fixtures"
DEFAULT_MANIFEST = ROOT / "tests/function_coverage/builtin_signatures_manifest.json"
FILE_GLOB = "builtin_signatures_*.stan"
TARGET_SIGNATURES_PER_MODEL = 240

def scalar_profile(signature: Signature, descriptor: RegistrySpec) \
        -> tuple[tuple[object, ...], float, tuple[int, ...]]:
    # A real-valued BuiltinSpec argument may be supplied by an integer
    # overload through Stan's ordinary promotion rules. Select values using
    # the semantic registry kind, then spell them as the dumped concrete type.
    # Otherwise profiles such as atanh's (-1, 1) domain receive the generic
    # integer center 2 and fail before the builtin route is exercised.
    arguments = tuple(AtomicType(value) for value in descriptor.arguments)
    result = AtomicType(descriptor.result)
    collapsed = Signature(signature.name, arguments, result,
                          signature.canonical_id)
    centers, _, perturbation, profile_data, _ = scalar_values(collapsed)
    if signature.name == "binary_log_loss":
        centers = (1, 0.4)
    elif signature.name == "choose":
        centers = (5, 2)
    elif descriptor.layout == "constructor":
        # Extent 1 keeps the constructed container the same size as the
        # unit-extent result declaration; bounds and indices stay valid.
        if signature.name.startswith("linspaced_"):
            centers = (1, 0, 1)
        elif signature.name.startswith("one_hot_"):
            centers = (1, 1)
        else:
            centers = (1,)
    elif descriptor.layout == "slice_view":
        # Every rendered container has unit extents, so index 1 (and count 1)
        # is the one in-bounds choice for each integer index argument.
        # Container arguments (appends take two) keep their profile centers.
        centers = tuple(
            1 if kind == "int" else center
            for center, kind in zip(centers, descriptor.arguments))
    elif (signature.arity == 1
          and numeric_leaf_kind(signature.arguments[0]) == "int"
          and descriptor.arguments[0] == "real"):
        zero_safe = {"acos", "asin", "atanh", "inv_Phi", "lambert_wm1",
                     "log1m", "logit", "std_normal_qf"}
        negative_safe = {"log1m_exp", "std_normal_log_qf"}
        centers = ((0 if signature.name in zero_safe else
                    -1 if signature.name in negative_safe else 1),)
    data_positions = set(profile_data)
    # Constructor arguments are data by contract: the underlying Stan Math
    # builders take doubles, so every rendered argument must be a literal.
    # Predicate arguments render as literals too: an integer-typed result
    # computed from parameter-dependent scalars would drag every context
    # onto the runtime-control machinery, which the dedicated stress models
    # cover; this inventory exercises registry dispatch and the shared
    # evaluator.
    if descriptor.layout in {"constructor", "predicate"}:
        data_positions.update(range(len(signature.arguments)))
    # Slice indexes must be compile-time integers in the graph and
    # register-machine backends, so render them as literals; container
    # arguments stay declared (parameter-dependent) values.
    if descriptor.layout == "slice_view":
        data_positions.update(
            index for index, kind in enumerate(descriptor.arguments)
            if index > 0 and kind == "int")
    for index, argument in enumerate(signature.arguments):
        _, data_only = unwrap_data(argument)
        if data_only:
            data_positions.add(index)
    return tuple(centers), perturbation, tuple(sorted(data_positions))


def render_case(signature: Signature, descriptor: RegistrySpec,
                case_index: int) -> str:
    # Signature compatibility is dimension-independent. Unit extents keep
    # rank-eight overloads executable in the same shard without turning this
    # inventory test into a duplicate multi-lane/indexing stress test.
    dimensions = tuple(1 for _ in _shape_dimensions(signature))
    centers, perturbation, data_positions = scalar_profile(signature, descriptor)
    declarations: list[str] = []
    arguments: list[str] = []
    for index, (wrapped_argument, center) in enumerate(
            zip(signature.arguments, centers)):
        argument, explicitly_data = unwrap_data(wrapped_argument)
        real_lane = 0

        def next_real() -> str:
            nonlocal real_lane
            offset = ELEMENT_OFFSETS[real_lane % len(ELEMENT_OFFSETS)]
            real_lane += 1
            base = float(center) + offset
            if index in data_positions or explicitly_data:
                return f"{base:.17g}"
            return f"({base:.17g} + {perturbation:.17g} * seed)"

        literal = _numeric_literal(argument, int(center), next_real, dimensions)
        if index in data_positions or explicitly_data:
            arguments.append(literal)
        else:
            name = f"arg_{index + 1}"
            declarations.append(
                f"{_type_declaration(argument, name, dimensions)} = {literal};")
            arguments.append(name)

    call = f"{signature.name}({', '.join(arguments)})"
    result, _ = unwrap_data(signature.result)
    result_dimensions = dimensions
    if signature.name == "dims":
        # dims() answers one integer per extent of its argument, so the
        # declared result length is the argument's rank, not a unit extent.
        argument, _ = unwrap_data(signature.arguments[0])
        rank = 0
        if isinstance(argument, ArrayType):
            rank += argument.rank
            argument = argument.element
        if isinstance(argument, AtomicType):
            # Stan Math's dims() pushes rows and columns for any Eigen
            # value, so a vector contributes two entries.
            rank += {"vector": 2, "row_vector": 2, "matrix": 2,
                     "complex_vector": 2, "complex_row_vector": 2,
                     "complex_matrix": 2}.get(argument.name, 0)
        result_dimensions = (rank,)
    elif signature.name.startswith("append_"):
        # Two unit-extent operands concatenate to extent two along the
        # appended axis: columns for append_col's matrix results, the first
        # declared axis otherwise (rows, a vector's length, or the outer
        # array dimension).
        axis = 1 if (signature.name == "append_col"
                     and isinstance(result, AtomicType)
                     and result.name == "matrix") else 0
        result_dimensions = tuple(
            2 if index == axis else 1
            for index in range(max(len(dimensions), axis + 1)))
    declarations.append(
        f"{_type_declaration(result, 'value', result_dimensions)} = {call};")
    weight = 1.0 + ((case_index * 7) % 17) / 64.0
    declarations.extend(observe_result(result, "value", weight))
    body = "\n".join(f"      {line}" for line in declarations)
    return (f"    // {signature.canonical_id}\n"
            f"    {{\n{body}\n    }}")


def observe_result(value: StanType, expression: str, weight: float,
                   depth: int = 0) -> list[str]:
    value, _ = unwrap_data(value)
    if isinstance(value, ArrayType):
        # Keep the assignment live without adding size/index operations to
        # the overload test. The condition is false at every lit point but is
        # parameter-dependent in model/generated-quantities lowering, so O1
        # cannot discard the typed value or its builtin call.
        return ["if (seed > 1e100)", f"  print({expression});"]
    if not isinstance(value, AtomicType):
        raise ValueError(f"cannot observe {value.canonical()}")
    reduced = f"sum({expression})" if value.name in {
        "vector", "row_vector", "matrix"} else expression
    return [f"total += {weight:.17g} * {reduced};"]


def partition(cases: Sequence[tuple[Signature, str]]) \
        -> list[list[tuple[Signature, str]]]:
    groups = balanced_partitions(cases, TARGET_SIGNATURES_PER_MODEL)
    for group in groups:
        group.sort(key=lambda value: value[0].canonical_id)
    return groups


def render_model(index: int, count: int,
                 cases: Sequence[tuple[Signature, str]]) -> str:
    function = f"builtin_signatures_{index:02d}"
    body = ("    real total = 0;\n" +
            "\n".join(source for _, source in cases) +
            "\n    return total;")
    return all_context_model(
        function, body,
        "Generated by tools/generate_builtin_signature_models.py from the "
        "unified FunctionSpec registry",
        f"Partition {index} of {count}; {len(cases)} overloads.")


def generate(stanc: pathlib.Path, registry: pathlib.Path,
             output_dir: pathlib.Path,
             manifest_path: pathlib.Path, check: bool) -> bool:
    descriptors = registry_by_name(registry, "builtin")
    inventory = load_inventory(stanc)
    selected: list[tuple[Signature, RegistrySpec]] = []
    excluded: list[dict[str, str]] = []
    dumped_names: set[str] = set()
    for signature in inventory.signatures:
        candidates = descriptors.get(signature.name)
        if candidates is None:
            continue
        dumped_names.add(signature.name)
        descriptor, reason = resolve_registry_spec(
            candidates,
            tuple(numeric_leaf_kind(value) for value in signature.arguments),
            numeric_leaf_kind(signature.result), signature.canonical_id)
        if descriptor is not None and descriptor.layout == "rng":
            # _rng calls are illegal inside the generated pure-function
            # contexts, and draws are nondeterministic; the seeded census
            # replay owns their coverage.
            excluded.append({"signature": signature.canonical_id,
                             "reason": "rng: covered by the seeded census "
                                       "replay"})
        elif descriptor is not None:
            selected.append((signature, descriptor))
        else:
            excluded.append({"signature": signature.canonical_id,
                             "reason": reason})
    selected.sort(key=lambda value: value[0].canonical_id)
    rendered = [(signature, render_case(signature, descriptor, index))
                for index, (signature, descriptor) in enumerate(selected, 1)]
    groups = partition(rendered)
    expected: dict[pathlib.Path, str] = {}
    for index, group in enumerate(groups, 1):
        path = output_dir / f"builtin_signatures_{index:02d}.stan"
        expected[path] = render_model(index, len(groups), group)

    models = []
    for path, source in expected.items():
        ids = [signature.canonical_id for signature, _ in
               groups[int(path.stem.rsplit('_', 1)[1]) - 1]]
        models.append(generated_model_record(path, source, ids, ROOT))
    manifest = {
        "generator": "tools/generate_builtin_signature_models.py",
        "stanc_build_id": inventory.stanc_build_id,
        "signature_dump_sha256": inventory.raw_sha256,
        "registry_name_count": len(descriptors),
        "dumped_registry_name_count": len(dumped_names),
        "tested_signature_count": len(selected),
        "excluded_signature_count": len(excluded),
        "missing_from_stanc": sorted(set(descriptors) - dumped_names),
        "excluded": excluded,
        "models": models,
    }
    okay = write_generated_outputs(expected, output_dir, FILE_GLOB,
                                    manifest_path, manifest, check, ROOT)
    if not check:
        print(f"generated {len(groups)} models covering {len(selected)} signatures")
    return okay


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stanc", type=pathlib.Path, default=DEFAULT_STANC)
    parser.add_argument("--registry", type=pathlib.Path,
                        default=DEFAULT_REGISTRY)
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--manifest", type=pathlib.Path,
                        default=DEFAULT_MANIFEST)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    return 0 if generate(arguments.stanc, arguments.registry,
                         arguments.output_dir,
                         arguments.manifest, arguments.check) else 1


if __name__ == "__main__":
    raise SystemExit(main())
