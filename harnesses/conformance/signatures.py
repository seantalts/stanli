"""stanc3 signature inventory and its recursive type grammar.

The signature dump contains commas at three different grammatical levels:
ordinary arguments, array ranks, and nested function/tuple types.  Keeping a
small parser here is both less code and substantially safer than teaching
every generator to split strings with its own collection of exceptions.
"""

from __future__ import annotations

import dataclasses
import hashlib
import pathlib
import subprocess
from typing import Callable, Dict, Iterable, Iterator, Optional, Sequence, Tuple


REAL_ATOMS = frozenset(
    {
        "real",
        "vector",
        "row_vector",
        "matrix",
        "complex",
        "complex_vector",
        "complex_row_vector",
        "complex_matrix",
    }
)
COMPLEX_ATOMS = frozenset(
    {"complex", "complex_vector", "complex_row_vector", "complex_matrix"}
)


class SignatureParseError(ValueError):
    """A dump line could not be parsed without guessing."""

    def __init__(self, message: str, text: str, offset: int = 0,
                 line_number: Optional[int] = None):
        self.text = text
        self.offset = offset
        self.line_number = line_number
        prefix = f"line {line_number}: " if line_number is not None else ""
        pointer = " " * max(0, offset) + "^"
        super().__init__(f"{prefix}{message}\n{text}\n{pointer}")


@dataclasses.dataclass(frozen=True)
class StanType:
    """Base class for the type tree emitted by stanc3."""

    def canonical(self) -> str:
        raise NotImplementedError

    def children(self) -> Tuple["StanType", ...]:
        return ()

    def walk(self) -> Iterator["StanType"]:
        yield self
        for child in self.children():
            yield from child.walk()

    def contains_atomic(self, names: Iterable[str]) -> bool:
        wanted = frozenset(names)
        return any(isinstance(node, AtomicType) and node.name in wanted
                   for node in self.walk())

    def contains_kind(self, kind: str) -> bool:
        return any(node.kind == kind for node in self.walk())

    @property
    def kind(self) -> str:
        raise NotImplementedError

    def has_numeric_lane(self) -> bool:
        """Whether a value of this type contains a target-able real lane.

        Complex and tuple values are included deliberately.  They require a
        capability policy or a generator which projects their real-valued
        components; silently calling them inapplicable would hide inventory.
        """
        return False

    def has_differentiable_lane(self) -> bool:
        """Whether a direct argument can be sourced from a real parameter."""
        return False

    def to_dict(self) -> Dict[str, object]:
        raise NotImplementedError


@dataclasses.dataclass(frozen=True)
class AtomicType(StanType):
    name: str

    @property
    def kind(self) -> str:
        return self.name

    def canonical(self) -> str:
        return self.name

    def has_numeric_lane(self) -> bool:
        return self.name in REAL_ATOMS

    def has_differentiable_lane(self) -> bool:
        return self.name in REAL_ATOMS

    def to_dict(self) -> Dict[str, object]:
        return {"kind": "atomic", "name": self.name}


@dataclasses.dataclass(frozen=True)
class ArrayType(StanType):
    rank: int
    element: StanType

    def __post_init__(self) -> None:
        if self.rank < 1:
            raise ValueError("Stan array rank must be positive")

    @property
    def kind(self) -> str:
        return "array"

    def canonical(self) -> str:
        return "array[" + "," * (self.rank - 1) + "]" + \
            self.element.canonical()

    def children(self) -> Tuple[StanType, ...]:
        return (self.element,)

    def has_numeric_lane(self) -> bool:
        return self.element.has_numeric_lane()

    def has_differentiable_lane(self) -> bool:
        return self.element.has_differentiable_lane()

    def to_dict(self) -> Dict[str, object]:
        return {
            "kind": "array",
            "rank": self.rank,
            "element": self.element.to_dict(),
        }


@dataclasses.dataclass(frozen=True)
class TupleType(StanType):
    elements: Tuple[StanType, ...]

    @property
    def kind(self) -> str:
        return "tuple"

    def canonical(self) -> str:
        return "tuple(" + ",".join(x.canonical() for x in self.elements) + ")"

    def children(self) -> Tuple[StanType, ...]:
        return self.elements

    def has_numeric_lane(self) -> bool:
        return any(x.has_numeric_lane() for x in self.elements)

    def has_differentiable_lane(self) -> bool:
        return any(x.has_differentiable_lane() for x in self.elements)

    def to_dict(self) -> Dict[str, object]:
        return {"kind": "tuple",
                "elements": [x.to_dict() for x in self.elements]}


@dataclasses.dataclass(frozen=True)
class DataType(StanType):
    value: StanType

    @property
    def kind(self) -> str:
        return "data"

    def canonical(self) -> str:
        return "data " + self.value.canonical()

    def children(self) -> Tuple[StanType, ...]:
        return (self.value,)

    def has_numeric_lane(self) -> bool:
        return self.value.has_numeric_lane()

    def has_differentiable_lane(self) -> bool:
        return False

    def to_dict(self) -> Dict[str, object]:
        return {"kind": "data", "value": self.value.to_dict()}


@dataclasses.dataclass(frozen=True)
class FunctionType(StanType):
    arguments: Tuple[StanType, ...]
    result: StanType

    @property
    def kind(self) -> str:
        return "function"

    def canonical(self) -> str:
        args = ",".join(x.canonical() for x in self.arguments)
        return "(" + args + ")=>" + self.result.canonical()

    def children(self) -> Tuple[StanType, ...]:
        return self.arguments + (self.result,)

    # A function value is supplied by declaring a UDF, not by an
    # unconstrained parameter.  Its own argument/result lanes therefore do
    # not establish a differentiable path for the outer signature.
    def to_dict(self) -> Dict[str, object]:
        return {
            "kind": "function",
            "arguments": [x.to_dict() for x in self.arguments],
            "result": self.result.to_dict(),
        }


@dataclasses.dataclass(frozen=True)
class Signature:
    name: str
    arguments: Tuple[StanType, ...]
    result: StanType
    raw: str

    @property
    def canonical_id(self) -> str:
        args = ",".join(x.canonical() for x in self.arguments)
        return f"{self.name}({args})=>{self.result.canonical()}"

    @property
    def case_id(self) -> str:
        return "signature:" + self.canonical_id

    @property
    def arity(self) -> int:
        return len(self.arguments)

    @property
    def is_probability(self) -> bool:
        return self.name.endswith(("_lpdf", "_lpmf", "_cdf", "_lcdf",
                                   "_lccdf"))

    @property
    def is_rng(self) -> bool:
        return self.name.endswith("_rng")

    @property
    def contains_complex(self) -> bool:
        nodes = self.arguments + (self.result,)
        return any(x.contains_atomic(COMPLEX_ATOMS) for x in nodes)

    @property
    def has_function_argument(self) -> bool:
        return any(isinstance(x, FunctionType) for x in self.arguments)

    def structural_inapplicability(self) -> Optional[str]:
        if self.is_rng:
            return "rng_only"
        if not self.result.has_numeric_lane():
            return "no_real_bearing_result"
        if not any(arg.has_differentiable_lane() for arg in self.arguments):
            return "no_differentiable_path"
        return None

    def to_dict(self) -> Dict[str, object]:
        return {
            "name": self.name,
            "canonical_id": self.canonical_id,
            "case_id": self.case_id,
            "raw": self.raw,
            "arguments": [x.to_dict() for x in self.arguments],
            "result": self.result.to_dict(),
        }


class _Parser:
    def __init__(self, text: str, line_number: Optional[int] = None):
        self.text = text
        self.pos = 0
        self.line_number = line_number

    def error(self, message: str) -> SignatureParseError:
        return SignatureParseError(message, self.text, self.pos,
                                   self.line_number)

    def skip_space(self) -> None:
        while self.pos < len(self.text) and self.text[self.pos].isspace():
            self.pos += 1

    def peek(self, value: str) -> bool:
        self.skip_space()
        return self.text.startswith(value, self.pos)

    def take(self, value: str) -> None:
        self.skip_space()
        if not self.text.startswith(value, self.pos):
            raise self.error(f"expected {value!r}")
        self.pos += len(value)

    def identifier(self) -> str:
        self.skip_space()
        start = self.pos
        if self.pos >= len(self.text) or not (
                self.text[self.pos].isalpha() or self.text[self.pos] == "_"):
            raise self.error("expected an identifier")
        self.pos += 1
        while self.pos < len(self.text) and (
                self.text[self.pos].isalnum() or self.text[self.pos] == "_"):
            self.pos += 1
        return self.text[start:self.pos]

    def type_list(self, closing: str) -> Tuple[StanType, ...]:
        values = []
        self.skip_space()
        if self.peek(closing):
            return ()
        while True:
            values.append(self.type())
            self.skip_space()
            if self.peek(closing):
                return tuple(values)
            self.take(",")

    def type(self) -> StanType:
        self.skip_space()
        if self.peek("("):
            self.take("(")
            args = self.type_list(")")
            self.take(")")
            self.take("=>")
            return FunctionType(args, self.type())

        word = self.identifier()
        if word == "data":
            return DataType(self.type())
        if word == "array":
            self.take("[")
            rank = 1
            self.skip_space()
            while self.peek(","):
                self.take(",")
                rank += 1
            self.take("]")
            return ArrayType(rank, self.type())
        if word == "tuple":
            self.take("(")
            elements = self.type_list(")")
            self.take(")")
            return TupleType(elements)
        return AtomicType(word)

    def signature(self) -> Signature:
        name = self.identifier()
        self.take("(")
        args = self.type_list(")")
        self.take(")")
        self.take("=>")
        result = self.type()
        self.skip_space()
        if self.pos != len(self.text):
            raise self.error("unexpected trailing text")
        return Signature(name, args, result, self.text)


def parse_signature(text: str, line_number: Optional[int] = None) -> Signature:
    """Parse one complete stanc3 dump line."""
    if not text.strip():
        raise SignatureParseError("empty signature", text, 0, line_number)
    return _Parser(text, line_number).signature()


@dataclasses.dataclass(frozen=True)
class Inventory:
    signatures: Tuple[Signature, ...]
    raw_dump: str
    stanc_build_id: str
    raw_sha256: str
    stanc_sha256: Optional[str] = None

    @property
    def signature_count(self) -> int:
        return len(self.signatures)

    @property
    def name_count(self) -> int:
        return len({sig.name for sig in self.signatures})

    def select(self, predicate: Callable[[Signature], bool]) -> "Inventory":
        return dataclasses.replace(
            self, signatures=tuple(x for x in self.signatures if predicate(x)))

    def to_metadata(self) -> Dict[str, object]:
        return {
            "stanc_build_id": self.stanc_build_id,
            "stanc_sha256": self.stanc_sha256,
            "raw_sha256": self.raw_sha256,
            "total_signatures": self.signature_count,
            "total_names": self.name_count,
            # Keeping the exact bytes (decoded as stanc's UTF-8 text) makes a
            # report independently auditable even after a nightly binary has
            # moved on.  Consumers which only need totals can ignore it.
            "raw_dump": self.raw_dump,
        }


def inventory_from_dump(raw_dump: str, stanc_build_id: str) -> Inventory:
    signatures = []
    by_id = {}
    for line_number, raw_line in enumerate(raw_dump.splitlines(), 1):
        if not raw_line.strip():
            continue
        signature = parse_signature(raw_line, line_number)
        previous = by_id.get(signature.canonical_id)
        if previous is not None:
            raise SignatureParseError(
                f"duplicate canonical signature; first seen on line {previous}",
                raw_line, 0, line_number)
        by_id[signature.canonical_id] = line_number
        signatures.append(signature)
    if not signatures:
        raise SignatureParseError("signature dump was empty", raw_dump, 0)
    digest = hashlib.sha256(raw_dump.encode("utf-8")).hexdigest()
    return Inventory(tuple(signatures), raw_dump, stanc_build_id, digest)


def _run(command: Sequence[str], timeout: float) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(command, capture_output=True, text=True,
                              timeout=timeout)
    except OSError as exc:
        raise RuntimeError(f"could not execute {command[0]}: {exc}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"timed out running {' '.join(command)}") from exc


def load_inventory(stanc: pathlib.Path, timeout: float = 120.0) -> Inventory:
    """Run the requested compiler and parse its authoritative inventory."""
    stanc = pathlib.Path(stanc)
    version = _run([str(stanc), "--version"], timeout)
    if version.returncode != 0:
        detail = (version.stderr or version.stdout).strip()
        raise RuntimeError(f"stanc --version failed ({version.returncode}): "
                           f"{detail}")
    build_id = (version.stdout or version.stderr).strip()
    if not build_id:
        raise RuntimeError("stanc --version returned no build identifier")

    dumped = _run([str(stanc), "--dump-stan-math-signatures"], timeout)
    if dumped.returncode != 0:
        detail = (dumped.stderr or dumped.stdout).strip()
        raise RuntimeError(
            "stanc --dump-stan-math-signatures failed "
            f"({dumped.returncode}): {detail}")
    inventory = inventory_from_dump(dumped.stdout, build_id)
    try:
        binary_digest = hashlib.sha256(stanc.read_bytes()).hexdigest()
    except OSError as exc:
        raise RuntimeError(f"could not hash stanc at {stanc}: {exc}") from exc
    return dataclasses.replace(inventory, stanc_sha256=binary_digest)
