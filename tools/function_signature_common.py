"""Shared registry and fixture machinery for exhaustive function coverage."""

from __future__ import annotations

import dataclasses
import json
import os
import math
import pathlib
import subprocess
from typing import Sequence, TypeVar

from conformance.signatures import ArrayType, AtomicType, DataType, StanType


@dataclasses.dataclass(frozen=True)
class RegistrySpec:
    name: str
    family: str
    opcode: int
    arity: int
    arguments: tuple[str, ...]
    result: str
    activity_mask: int
    evaluation: str
    layout: str


def load_registry(executable: pathlib.Path) -> tuple[RegistrySpec, ...]:
    completed = subprocess.run(
        [str(executable)], check=True, capture_output=True, text=True)
    raw = json.loads(completed.stdout)
    return tuple(RegistrySpec(
        name=item["name"], family=item["family"], opcode=item["opcode"],
        arity=item["arity"], arguments=tuple(item["arguments"]),
        result=item["result"], activity_mask=item["activity_mask"],
        evaluation=item["evaluation"], layout=item["layout"],
    ) for item in raw)


def registry_by_name(executable: pathlib.Path, family: str) \
        -> dict[str, tuple[RegistrySpec, ...]]:
    grouped: dict[str, list[RegistrySpec]] = {}
    for spec in load_registry(executable):
        if spec.family != family:
            continue
        entries = grouped.setdefault(spec.name, [])
        if spec in entries:
            raise ValueError(f"duplicate registry entry for {spec.name}")
        entries.append(spec)
    return {name: tuple(entries) for name, entries in grouped.items()}


def unwrap_data(value: StanType) -> tuple[StanType, bool]:
    data_only = False
    while isinstance(value, DataType):
        data_only = True
        value = value.value
    return value, data_only


def numeric_leaf_kind(value: StanType) -> str | None:
    value, _ = unwrap_data(value)
    while isinstance(value, ArrayType):
        value, _ = unwrap_data(value.element)
    if not isinstance(value, AtomicType):
        return None
    if value.name == "int":
        return "int"
    if value.name in {"real", "vector", "row_vector", "matrix"}:
        return "real"
    return None


def resolve_registry_spec(
        candidates: Sequence[RegistrySpec],
        arguments: Sequence[str | None], result: str | None,
        identity: str) -> tuple[RegistrySpec | None, str | None]:
    """Select the most specific numeric registry overload for a signature."""
    def incompatibility(spec: RegistrySpec) -> str | None:
        if len(arguments) != spec.arity:
            return "arity mismatch"
        if any(value is None for value in arguments):
            return "unsupported argument type"
        if any(expected == "int" and found != "int"
               for found, expected in zip(arguments, spec.arguments)):
            return "integer argument contract mismatch"
        if result != spec.result:
            return "result kind mismatch"
        return None

    matches = [spec for spec in candidates if incompatibility(spec) is None]
    if not matches:
        reasons = {incompatibility(spec) for spec in candidates}
        reason = (next(iter(reasons)) if len(reasons) == 1 else
                  "no compatible registry overload")
        return None, reason

    def promotions(spec: RegistrySpec) -> int:
        return sum(found == "int" and expected == "real"
                   for found, expected in zip(arguments, spec.arguments))

    best_promotions = min(map(promotions, matches))
    best = [spec for spec in matches
            if promotions(spec) == best_promotions]
    if len(best) != 1:
        raise ValueError(f"ambiguous registry overload for {identity}")
    return best[0], None


T = TypeVar("T")


def balanced_partitions(items: Sequence[tuple[T, str]], target_count: int) \
        -> list[list[tuple[T, str]]]:
    if not items:
        return []
    group_count = math.ceil(len(items) / target_count)
    groups: list[list[tuple[T, str]]] = [[] for _ in range(group_count)]
    sizes = [0] * group_count
    for item in sorted(items, key=lambda value: -len(value[1])):
        candidates = [index for index, group in enumerate(groups)
                      if len(group) < target_count]
        selected = min(candidates, key=lambda index: (sizes[index], index))
        groups[selected].append(item)
        sizes[selected] += len(item[1])
    return groups


def all_context_model(function_name: str, body: str, provenance: str,
                      partition: str = "", timeout: int = 180) -> str:
    partition_line = f"// {partition}\n" if partition else ""
    return f"""// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {{\"context_seed\": 0.0}}
// STANLI-LIT-TIMEOUT: {timeout}
// {provenance}
// and `stanc --dump-stan-math-signatures`. Do not edit by hand.
{partition_line}functions {{
  real {function_name}(real seed) {{
{body}
  }}
}}
data {{
  real context_seed;
}}
transformed data {{
  real transformed_data_result = {function_name}(context_seed);
}}
parameters {{
  real probe;
}}
model {{
  probe ~ std_normal();
  target += {function_name}(probe);
  if (probe > -1e100)
    target += {function_name}(probe);
}}
generated quantities {{
  real generated_quantities_result = {function_name}(probe);
}}
"""


def write_or_check(path: pathlib.Path, content: str, check: bool,
                   touch_unchanged: bool = True) -> bool:
    if check:
        if not path.exists() or path.read_text() != content:
            print(f"stale generated file: {path}")
            return False
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == content:
        # The models are build-rule outputs whose timestamps must advance
        # when the rule runs; the manifests are configure inputs whose
        # timestamps must move only on real content changes, or every
        # build would re-run configuration.
        if touch_unchanged:
            os.utime(path, None)
        return True
    path.write_text(content)
    return True


def display_path(path: pathlib.Path, root: pathlib.Path) -> str:
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path.resolve())


def generated_model_record(path: pathlib.Path, source: str,
                           signatures: Sequence[str], root: pathlib.Path) \
        -> dict[str, object]:
    import hashlib
    return {
        "file": display_path(path, root),
        "signature_count": len(signatures),
        "source_bytes": len(source.encode("utf-8")),
        "source_sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
        "signatures": list(signatures),
    }


def write_generated_outputs(
        expected: dict[pathlib.Path, str], output_dir: pathlib.Path,
        file_glob: str, manifest_path: pathlib.Path,
        manifest: dict[str, object], check: bool,
        root: pathlib.Path) -> bool:
    okay = True
    for path, source in expected.items():
        okay = write_or_check(path, source, check) and okay
    stale = set(output_dir.glob(file_glob)) - set(expected)
    if stale:
        if check:
            for path in sorted(stale):
                print(f"unexpected generated file: {display_path(path, root)}")
            okay = False
        else:
            for path in stale:
                path.unlink()
    manifest_source = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    return write_or_check(manifest_path, manifest_source, check,
                          touch_unchanged=False) and okay
